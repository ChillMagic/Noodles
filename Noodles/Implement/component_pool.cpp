#include "component_pool.h"
#include "../../Potato/tool.h"
namespace Noodles::Implement
{
	static constexpr size_t min_page_comp_count = 32;

	bool TypeLayoutArray::operator<(const TypeLayoutArray& input) const noexcept
	{
		if (count < input.count) return true;
		else if (count == input.count)
		{
			for (size_t i = 0; i < count; ++i)
			{
				if (layouts[i] < input[i])
					return true;
				else if (!(layouts[i] == input[i]))
					break;
			}
		}
		return false;
	}

	bool TypeLayoutArray::operator==(const TypeLayoutArray& input) const noexcept
	{
		if (layouts == input.layouts)
			return true;
		else if (count == input.count)
		{
			for (size_t i = 0; i < count; ++i)
				if (layouts[i] != input[i])
					return true;
			return false;
		}
		else
			return false;
	}

	bool TypeLayoutArray::locate_ordered(const TypeInfo* input, size_t* output, size_t length) const noexcept
	{
		if (count >= length)
		{
			size_t i = 0, k = 0;
			while (i < count && k < length)
			{
				if (layouts[i] == input[k])
				{
					if(output != nullptr)
						output[k] = i;
					++i, ++k;
				}
				else if (layouts[i] < input[k])
					++i;
				else
					return false;
			}
			return k == length;
		}
		return false;
	}

	size_t TypeLayoutArray::locate(const TypeInfo& input) const noexcept
	{
		size_t i = 0;
		while (i < count)
		{
			if (layouts[i] < input)
				++i;
			else if (layouts[i] == input)
				return i;
			else
				return count;
		}
		return i;
	}

	bool TypeLayoutArray::locate_unordered(const TypeInfo* input, size_t* output, size_t length) const noexcept
	{
		for (size_t i = 0; i < length; ++i)
		{
			size_t index = locate(input[i]);
			if (index >= count)
				return false;
			else if (output != nullptr)
				output[i] = index;
		}
		return true;
	}

	StorageBlock* create_storage_block(MemoryPageAllocator& allocator, const TypeGroup* owner)
	{
		auto [buffer, page_size] = allocator.allocate(owner->page_size());
		assert(page_size == owner->page_size());
		size_t element_count = owner->element_count();
		auto [layouts, layout_count] = owner->layouts();
		StorageBlock* result = new (buffer) StorageBlock{};
		result->m_owner = owner;
		result->datas = reinterpret_cast<void**>(result + 1);
		buffer = reinterpret_cast<std::byte*>(result->datas + layout_count);
		page_size -= sizeof(StorageBlock) +  sizeof(void*) * layout_count;
		for (size_t i = 0; i < layout_count; ++i)
		{
			void* ptr = buffer;
			auto result_r = std::align(layouts[i].align, layouts[i].size * element_count, ptr, page_size);
			assert(result_r != nullptr);
			result->datas[i] = ptr;
			buffer = reinterpret_cast<std::byte*>(ptr);
			buffer += layouts[i].size * element_count;
			page_size -= layouts[i].size * element_count;
		}

		{
			void* ptr = buffer;
			std::align(alignof(EntityInterface*), sizeof(EntityInterface*), ptr, page_size);
			buffer = reinterpret_cast<std::byte*>(ptr);
		}
		result->entitys = reinterpret_cast<EntityInterface * *>(buffer);
		for (size_t i = 0; i < element_count; ++i)
			result->entitys[i] = nullptr;
		buffer = reinterpret_cast<std::byte*>(result->entitys + element_count);
		result->functions = reinterpret_cast<StorageBlockFunctionPair * *>(buffer);
		StorageBlockFunctionPair* tem_ptr = reinterpret_cast<StorageBlockFunctionPair*>(result->functions + layout_count);
		for (size_t i = 0; i < layout_count; ++i)
		{
			result->functions[i] = tem_ptr;
			tem_ptr += element_count;
		}
		return result;
	}

	void release_storage_block(StorageBlock* input, size_t index)
	{
		for (size_t i = 0; i < input->m_owner->layouts().count; ++i)
			input->functions[i][index].destructor(
				reinterpret_cast<std::byte*>(input->datas[i]) + input->m_owner->layouts()[i].size * index
			);
		auto& entity = input->entitys[index];
		if (entity != nullptr)
		{
			entity->set(nullptr, nullptr, 0);
			entity->sub_ref();
			entity = nullptr;
		}
	}

	void free_storage_block(StorageBlock* input) noexcept
	{
		for (size_t l = 0; l < input->m_owner->layouts().count; ++l)
		{
			auto function = input->functions[l];
			auto data = input->datas[l];
			auto layout_size = input->m_owner->layouts()[l].size;
			for (size_t i = 0; i < input->available_count; ++i)
				function[i].destructor(reinterpret_cast<std::byte*>(data) + layout_size * i);
		}
		for (size_t i = 0; i < input->available_count; ++i)
		{
			auto& entity = input->entitys[i];
			if (entity != nullptr)
			{
				entity->set(nullptr, nullptr, 0);
				entity->sub_ref();
				entity = nullptr;
			}
		}
		MemoryPageAllocator::release(reinterpret_cast<std::byte*>(input));
	}

	void TypeGroup::remove_page_from_list(StorageBlock* block)
	{
		auto front = block->front;
		auto next = block->next;
		if (front != nullptr)
			front->next = next;
		else
			m_start_block = next;
		if (next != nullptr)
			next->front = front;
		else
			m_last_block = front;
		block->next = nullptr;
		block->front = nullptr;
	}

	void TypeGroup::insert_page_to_list(StorageBlock* block)
	{
		if (m_start_block == nullptr)
		{
			m_start_block = block;
			m_last_block = block;
		}
		else {
			m_last_block->next = block;
			block->front = m_last_block;
			m_last_block = block;
		}
	}

	std::tuple<StorageBlock*, size_t> TypeGroup::allocate_group(MemoryPageAllocator& allocator)
	{
		++m_available_count;
		if (!m_deleted_page.empty())
		{
			auto min = m_deleted_page.end();
			for (auto ite = m_deleted_page.begin(); ite != m_deleted_page.end(); ++ite)
			{
				if (min != m_deleted_page.end())
				{
					if (min->second > ite->second)
					{
						assert(ite->second > 0);
						min = ite;
						if (ite->second == 1)
							break;
					}
				}
				else {
					min = ite;
					if (min->second == 1)
						break;
				}
			}
			assert(min != m_deleted_page.end());
			min->second -= 1;
			auto [block, index] = *min;
			if (index == 0)
				m_deleted_page.erase(min);
			for (size_t i = 0; block->available_count; ++i)
				if (block->entitys[i] == nullptr)
					return {block, i};
			assert(false);
			return {nullptr, 0};
		}
		else {
			if (m_start_block == nullptr || m_last_block->available_count == element_count())
				insert_page_to_list(create_storage_block(allocator, this));
			size_t index = m_last_block->available_count;
			++m_last_block->available_count;
			return { m_last_block , index};
		}
		
	}

	void TypeGroup::inside_move(StorageBlock* source, size_t sindex, StorageBlock* target, size_t tindex)
	{
		assert(target->entitys[tindex] != nullptr);
		assert(source->entitys[sindex] == nullptr);
		for (size_t i = 0; i < m_type_layouts.count; ++i)
		{
			auto& s_function = source->functions[i];
			auto& e_function= target->functions[i];
			size_t component_size = m_type_layouts[i].size;
			auto& e_fun = e_function[tindex];
			auto& s_fun = s_function[sindex];
			s_fun = e_fun;
			s_fun.mover(
				reinterpret_cast<std::byte*>(source->datas[i]) + component_size * sindex,
				reinterpret_cast<std::byte*>(target->datas[i]) + component_size * tindex
			);
		}
		source->entitys[sindex] = target->entitys[tindex];
		target->entitys[tindex] = nullptr;
		source->entitys[sindex]->set(this, source, sindex);
		release_storage_block(target, tindex);
	}

	void TypeGroup::release_group(StorageBlock* block, size_t index)
	{
		assert(index < element_count());
		--m_available_count;
		release_storage_block(block, index);
		auto ite = m_deleted_page.insert({ block, 0 }).first;
		++ite->second;
		if (block->available_count == ite->second)
		{
			remove_page_from_list(block);
			block->available_count = 0;
			free_storage_block(block);
			m_deleted_page.erase(ite);
		}
	}

	size_t backward_search(Implement::EntityInterface** start, size_t start_index, size_t end_index)
	{
		while (start_index < end_index)
		{
			if (start[start_index] == nullptr)
				return start_index;
			else
				++start_index;
		}
		return start_index;
	}

	size_t forward_search(Implement::EntityInterface** start, size_t start_index, size_t end_index)
	{
		while (start_index > end_index)
		{
			if (start[start_index - 1] != nullptr)
				return start_index;
			else
				--start_index;
		}
		return start_index;
	}

	void TypeGroup::update()
	{
		if (!m_deleted_page.empty())
		{
			assert(m_last_block != nullptr);
			std::deque<std::pair<StorageBlock*, size_t>> all_block;
			size_t last_page_deleted = 0;
			for (auto& ite : m_deleted_page)
			{
				if (ite.first != m_last_block)
				{
					remove_page_from_list(ite.first);
					all_block.push_back(ite);
				}
				else {
					assert(last_page_deleted == 0);
					last_page_deleted = ite.second;
				}
			}
			m_deleted_page.clear();
			last_page_deleted += element_count() - m_last_block->available_count;
			m_last_block->available_count = element_count();
			
			if (last_page_deleted != 0) {
				all_block.push_back({ m_last_block , last_page_deleted });
				remove_page_from_list(m_last_block);
			}
			std::sort(all_block.begin(), all_block.end(), [](const std::tuple<StorageBlock*, size_t>& in, const std::tuple<StorageBlock*, size_t>& in2) -> bool {
				return std::get<1>(in) < std::get<1>(in2);
			});
			assert(!all_block.empty());
			size_t start_i = 0, end_i = element_count();
			while (all_block.size() > 1)
			{
				auto start = all_block.begin();
				auto end = all_block.end() - 1;
				auto& s_entitys = start->first->entitys;
				auto& e_entitys = end->first->entitys;
				while (true)
				{
					start_i = backward_search(s_entitys, start_i, element_count());
					if (start_i != element_count())
					{
						end_i = forward_search(e_entitys, end_i, 0);
						if (end_i != 0)
						{
							inside_move(start->first, start_i, end->first, end_i - 1);
						}
						else {
							end_i = element_count();
							end->first->available_count = 0;
							free_storage_block(end->first);
							all_block.pop_back();
							break;
						}
					}
					else {
						start_i = 0;
						insert_page_to_list(start->first);
						all_block.pop_front();
						break;
					}
				}
			}
			auto cur = all_block.begin();
			auto& entitys = cur->first->entitys;

			while (true)
			{
				start_i = backward_search(entitys, start_i, end_i);
				end_i = forward_search(entitys, end_i, start_i);
				if (start_i + 1 < end_i)
					inside_move(cur->first, start_i, cur->first, end_i - 1);
				else
					break;
			}

			cur->first->available_count = start_i;
			if (cur->first->available_count != 0)
				insert_page_to_list(cur->first);
			else
				free_storage_block(cur->first);
			all_block.clear();
		}
	}

	TypeGroup* TypeGroup::create(TypeLayoutArray array)
	{
		size_t total_size = sizeof(TypeGroup) + array.count * sizeof(TypeInfo);
		std::byte* data = new std::byte[total_size];
		TypeInfo* layout = reinterpret_cast<TypeInfo*>(data + sizeof(TypeGroup));
		for (size_t i = 0; i < array.count; ++i)
			new (layout + i) TypeInfo{array.layouts[i]};
		TypeLayoutArray layouts{ layout , array.count};
		TypeGroup* result = new (data) TypeGroup{layouts};
		return result;
	}

	void TypeGroup::free(TypeGroup* input)
	{
		assert(input != nullptr);
		auto layouts = input->layouts();
		input->~TypeGroup();
		for (size_t i = 0; i < layouts.count; ++i)
			layouts[i].~TypeInfo();
		delete[] reinterpret_cast<std::byte*>(input);
	}

	TypeGroup::~TypeGroup()
	{
		while (m_start_block != nullptr)
		{
			auto tem = m_start_block;
			m_start_block = m_start_block->next;
			free_storage_block(tem);
		}
	}

	TypeGroup::TypeGroup(TypeLayoutArray input)
		: m_type_layouts(input)
	{
		size_t all_size = 0;
		size_t all_align = 0;
		for (size_t i = 0; i < m_type_layouts.count; ++i)
		{
			all_size += m_type_layouts.layouts[i].size;
			auto align_size = (m_type_layouts.layouts[i].align > alignof(nullptr_t)) ? m_type_layouts.layouts[i].align : alignof(nullptr_t);
			all_align += align_size;
		}
		size_t element_size = all_size + sizeof(EntityInterface*) + sizeof(StorageBlockFunctionPair) * m_type_layouts.count;
		size_t fixed_size = sizeof(StorageBlock) + (sizeof(StorageBlockFunctionPair*) + sizeof(void*)) * m_type_layouts.count + all_align + alignof(nullptr_t);
		m_page_size = fixed_size + element_size * min_page_comp_count;
		size_t bound_size = 1024 * 8 - MemoryPageAllocator::reserved_size();
		m_page_size = (m_page_size > bound_size) ? m_page_size : bound_size;
		std::tie(m_page_size, std::ignore) = MemoryPageAllocator::pre_calculte_size(m_page_size);
		m_element_count = (m_page_size - fixed_size) / element_size;
	}

	ComponentPool::InitBlock::~InitBlock()
	{
		if (start_block != nullptr)
		{
			MemoryPageAllocator::release(start_block);
		}
	}

	ComponentPool::InitHistory::~InitHistory()
	{
		if (ope == EntityOperator::Construct)
		{
			assert(data != nullptr);
			functions.destructor(data);
		}
	}

	void ComponentPool::construct_component(
		const TypeInfo& layout, void(*constructor)(void*, void*), void* data,
		EntityInterface* entity, void(*deconstructor)(void*) noexcept, void(*mover)(void*, void*) noexcept
	)
	{
		std::lock_guard lg(m_init_lock);
		size_t aligned_size = layout.align > sizeof(nullptr_t) ? layout.align - sizeof(nullptr_t) : 0;
		if (m_init_block.empty() || m_init_block.rbegin()->last_available_count < aligned_size + layout.size)
		{
			size_t allocate_size = 1024 * 16 - MemoryPageAllocator::reserved_size();
			assert(allocate_size > aligned_size + layout.size);
			auto [buffer, size] = m_allocator.allocate(allocate_size);
			m_init_block.emplace_back(buffer, buffer, size);
		}

		assert(!m_init_block.empty());
		auto& [head, last, size] = *m_init_block.rbegin();
		auto result = std::align(layout.align, layout.size, last, size);
		assert(result != nullptr);
		assert(entity != nullptr);
		constructor(last, data);
		EntityInterfacePtr ptr(entity);
		m_init_history[ptr].emplace_back(EntityOperator::Construct, layout, StorageBlockFunctionPair{deconstructor, mover}, last );
		last = reinterpret_cast<std::byte*>(last) + layout.size;
		size -= layout.size;
	}

	void ComponentPool::deconstruct_component(EntityInterface* entity, const TypeInfo& layout) noexcept
	{
		assert(entity != nullptr);
		std::lock_guard lg(m_init_lock);
		EntityInterfacePtr ptr(entity);
		m_init_history[ptr].emplace_back(EntityOperator::Destruct, layout, StorageBlockFunctionPair{nullptr, nullptr}, nullptr);
	}

	ComponentPool::ComponentPool(MemoryPageAllocator& allocator) noexcept : m_allocator(allocator){}

	void ComponentPool::clean_all()
	{
		std::lock_guard lg(m_init_lock);
		m_init_history.clear();
		m_init_block.clear();
		std::unique_lock ul(m_type_group_mutex);
		for (auto& ite : m_data)
			TypeGroup::free(ite.second);
		m_data.clear();
	}

	void ComponentPool::handle_entity_imp(EntityInterface* entity, EntityOperator ope) noexcept
	{
		assert(entity != nullptr);
		std::lock_guard lg(m_init_lock);
		m_init_history[entity].emplace_back(ope, TypeInfo::create<int>(), StorageBlockFunctionPair{ nullptr, nullptr }, nullptr);
	}


	bool ComponentPool::update()
	{
		std::lock_guard lg(m_init_lock);
		std::unique_lock ul(m_type_group_mutex);
		bool new_type_group = false;
		for (auto& ite : m_init_history)
		{
			Implement::TypeGroup* old_type_group;
			Implement::StorageBlock* old_storage_block;
			size_t old_element_index;
			ite.first->read(old_type_group, old_storage_block, old_element_index);
			std::map<TypeInfo, std::variant<size_t, InitHistory*>> old_type_template;
			if (old_type_group != nullptr)
			{
				assert(old_storage_block != nullptr);
				assert(old_element_index < old_type_group->element_count());
				for (size_t i = 0; i < old_type_group->layouts().count; ++i)
					old_type_template.insert({ old_type_group->layouts()[i], i });
			}
			for (auto& ite2 : ite.second)
			{
				bool need_destory = false;
				switch (ite2.ope)
				{
				case EntityOperator::Construct:
					old_type_template[ite2.type] = &ite2;
					break;
				case EntityOperator::Destruct:
					old_type_template.erase(ite2.type);
					break;
				case EntityOperator::Destory:
					need_destory = true;
				case EntityOperator::DeleteAll:
					old_type_template.clear();
					break;
				}
				if (need_destory)
					break;
			}
			if (!old_type_template.empty())
			{
				std::vector<TypeInfo> new_type_template;
				std::vector<std::variant<size_t, InitHistory*>> new_type_state_template;
				new_type_state_template.clear();
				for (auto& ite2 : old_type_template)
				{
					new_type_template.push_back(ite2.first);
					new_type_state_template.push_back(ite2.second);
				}
				auto find_result = m_data.find({ new_type_template.data(), new_type_template.size() });
				if (find_result == m_data.end())
				{
					TypeGroup* ptr = TypeGroup::create({ new_type_template.data(), new_type_template.size() });
					auto re = m_data.insert({ ptr->layouts(), ptr });
					assert(re.second);
					find_result = re.first;
					new_type_group = true;
				}
				if (find_result->second == old_type_group)
				{
					assert(old_type_group != nullptr);
					std::vector<bool> state_template;
					state_template.clear();
					state_template.resize(find_result->first.count, false);
					for (auto ite2 = ite.second.rbegin(); ite2 != ite.second.rend(); ++ite2)
					{
						if (ite2->ope == EntityOperator::Construct)
						{
							size_t type_index = old_type_group->layouts().locate(ite2->type);
							assert(type_index < state_template.size());
							if (!state_template[type_index])
							{
								auto& function = old_storage_block->functions[type_index][old_element_index];
								auto data = reinterpret_cast<std::byte*>(old_storage_block->datas[type_index]) + new_type_template[type_index].size * old_element_index;
								function.destructor(data);
								ite2->functions.mover(data, ite2->data);
								function = ite2->functions;
								state_template[type_index] = true;
							}
						}
					}
				}
				else {
					auto [new_block, new_element_index] = find_result->second->allocate_group(m_allocator);
					for (size_t i = 0; i < new_type_template.size(); ++i)
					{
						size_t component_size = new_type_template[i].size;
						auto& functions = new_block->functions[i][new_element_index];
						auto data = reinterpret_cast<std::byte*>(new_block->datas[i]) + component_size * new_element_index;
						auto& var = new_type_state_template[i];
						if (std::holds_alternative<size_t>(var))
						{
							size_t target_index = std::get<size_t>(var);
							assert(old_type_group != nullptr);
							auto source_function = old_storage_block->functions[target_index][old_element_index];
							auto source_data = reinterpret_cast<std::byte*>(old_storage_block->datas[target_index]) + component_size * old_element_index;
							functions = source_function;
							functions.mover(data, source_data);
						}
						else if (std::holds_alternative<InitHistory*>(var))
						{
							InitHistory* source = std::get<InitHistory*>(var);
							assert(source->ope == EntityOperator::Construct);
							functions = source->functions;
							functions.mover(data, source->data);
						}
					}
					auto& entitys = new_block->entitys[new_element_index];
					entitys = ite.first;
					entitys->add_ref();
					ite.first->set(find_result->second, new_block, new_element_index);
					if (old_type_group != nullptr)
					{
						auto& old_entitys = old_storage_block->entitys[old_element_index];
						old_entitys->sub_ref();
						old_entitys = nullptr;
						old_type_group->release_group(old_storage_block, old_element_index);
					}

				}
			}
			else {
				if (old_type_group != nullptr)
				{
					old_type_group->release_group(old_storage_block, old_element_index);
				}
			}
		}
		m_init_block.clear();
		m_init_history.clear();
		for (auto& ite : m_data)
			ite.second->update();
		return new_type_group;
	}

	void ComponentPool::search_type_group(
		const TypeInfo* require_tl, size_t require_tl_count,
		TypeGroup** output_tg,
		size_t* output_tl_index
	) const noexcept
	{
		size_t k = 0;
		for (auto& ite : m_data)
		{
			if (ite.first.locate_unordered(require_tl, output_tl_index + k * require_tl_count, require_tl_count))
				output_tg[k] = ite.second;
			else
				output_tg[k] = nullptr;
			++k;
		}
	}

	size_t ComponentPool::find_top_block(TypeGroup** tg, StorageBlock** output, size_t length) const noexcept
	{
		size_t data_count = m_data.size();
		assert(length == data_count);
		size_t total = 0;
		for (size_t i = 0; i < length; ++i)
		{
			if (tg[i] != nullptr)
			{
				output[i] = tg[i]->top_block();
				total += tg[i]->available_count();
			}
			else {
				output[i] = nullptr;
			}
		}
		return total;
	}

	size_t ComponentPool::type_group_count() const noexcept
	{
		return m_data.size();
	}

	void ComponentPool::update_type_group_state(std::vector<bool>& tar)
	{
		tar.resize(m_data.size());
		size_t i = 0;
		for (auto& ite : m_data)
			tar[i++] = (ite.second->top_block() != nullptr);
	}


	ComponentPool::~ComponentPool()
	{
		clean_all();
	}
}