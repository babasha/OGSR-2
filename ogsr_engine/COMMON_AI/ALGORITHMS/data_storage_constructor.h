////////////////////////////////////////////////////////////////////////////
//	Module 		: data_storage_constructor.h
//	Created 	: 21.03.2002
//  Modified 	: 28.02.2004
//	Author		: Dmitriy Iassenev
//	Description : Data storage constructor
////////////////////////////////////////////////////////////////////////////

#pragma once

template <typename T>
class CEmptyClassTemplate
{};
template <typename T1, typename T2>
class CEmptyClassTemplate2
{};

#include "manager_builder_allocator_constructor.h"

template <typename _algorithm, typename _manager, typename _builder, typename _allocator, template <typename> class _vertex = CEmptyClassTemplate,
          template <typename, typename> class _builder_allocator_constructor = CBuilderAllocatorConstructor,
          template <typename, typename, typename, template <typename, typename> class> class _manager_builder_allocator_constructor =
              CManagerBuilderAllocatorConstructor>
struct CDataStorageConstructor
    : public _algorithm::template CDataStorage<_manager_builder_allocator_constructor<_manager, _builder, _allocator, _builder_allocator_constructor>, _vertex>
{
    typedef typename _algorithm::template CDataStorage<_manager_builder_allocator_constructor<_manager, _builder, _allocator, _builder_allocator_constructor>, _vertex> inherited;

    typedef typename inherited::CGraphVertex CGraphVertex;
    typedef typename CGraphVertex::_index_type _index_type;

    IC CDataStorageConstructor(const u32 vertex_count) : inherited(vertex_count) {}
};
