#pragma once

#include <array>
#include <format>
#include <spdlog/spdlog.h>
#include "src/catalog/schema.h"
#include "src/type/tuple.h"
#include "src/type/value.h"


using neon::type::Row;
using neon::type::Value;


namespace neon::index
{

class BPTree
{
public:
    static constexpr int Degree = 5;
    static constexpr int PageSize = 4096;
    struct Node
    {
        char Page[PageSize];

        
    };
    Node * root;
    type::ValueType primary_key_type;
    catalog::SchemaRef schema;

    void insert(const Value & key, const Row & row) { }

private:
    static void to_node_string(Node * node, int level, std::string & prefix) { }
    static void split_child(Node * parent, int index, Node * child) { }

    static void insert_non_full(Node * node, const Value & key, const Row & row) { }
};

}
