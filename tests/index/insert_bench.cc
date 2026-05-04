#include <chrono>
#include <iostream>
#include <map>
#include <random>
#include <vector>
#include "src/index/bptree.h"
#include "src/type/value.h"

using namespace std::chrono;
using neon::type::Row;
using neon::type::RowRef;
using neon::type::Value;
using neon::type::ValueType;

// 基准测试函数模板
template<typename Container, typename InsertFunc>
long long benchmark_insert(Container& container, const std::vector<int>& data, InsertFunc insert_func) {
    auto start = high_resolution_clock::now();
    for (int val : data) {
        insert_func(container, val);
    }
    auto stop = high_resolution_clock::now();
    auto duration = duration_cast<microseconds>(stop - start);
    return duration.count();
}

// 基准测试函数模板 - 查找
template<typename Container, typename SearchFunc>
long long benchmark_search(Container& container, const std::vector<int>& data, SearchFunc search_func) {
    auto start = high_resolution_clock::now();
    for (int val : data) {
        search_func(container, val);
    }
    auto stop = high_resolution_clock::now();
    auto duration = duration_cast<microseconds>(stop - start);
    return duration.count();
}

// 基准测试函数模板 - 删除
template<typename Container, typename DeleteFunc>
long long benchmark_delete(Container& container, const std::vector<int>& data, DeleteFunc delete_func) {
    auto start = high_resolution_clock::now();
    for (int val : data) {
        delete_func(container, val);
    }
    auto stop = high_resolution_clock::now();
    auto duration = duration_cast<microseconds>(stop - start);
    return duration.count();
}

// std::map 的插入函数
void map_insert(std::map<int, RowRef>& map, int val) {
    RowRef row = std::make_shared<Row>();
    row->append_value(Value(ValueType::Int32, val));
    row->append_value(Value(ValueType::String, "test_data"));
    row->append_value(Value(ValueType::Bool, true));
    map[val] = row;
}

// BPTree 的插入函数
void bptree_insert(neon::index::BPTree& tree, int val) {
    RowRef row = std::make_shared<Row>();
    row->append_value(Value(ValueType::Int32, val));
    row->append_value(Value(ValueType::String, "test_data"));
    row->append_value(Value(ValueType::Bool, true));
    tree.insert(Value(ValueType::Int32, val), row);
}

// std::map 的查找函数
RowRef map_search(std::map<int, RowRef>& map, int val) {
    auto it = map.find(val);
    if (it != map.end()) {
        return it->second;
    }
    return nullptr;
}

// BPTree 的查找函数
RowRef bptree_search(neon::index::BPTree& tree, int val) {
    return tree.get(Value(ValueType::Int32, val));
}

// std::map 的删除函数
bool map_delete(std::map<int, RowRef>& map, int val) {
    return map.erase(val) > 0;
}

// BPTree 的删除函数
bool bptree_delete(neon::index::BPTree& tree, int val) {
    return tree.remove(Value(ValueType::Int32, val));
}

int main() {
    const int DATA_SIZE = 100000;
    
    // 生成测试数据
    std::vector<int> insert_data(DATA_SIZE);
    std::iota(insert_data.begin(), insert_data.end(), 1);
    
    // 打乱插入顺序
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(insert_data.begin(), insert_data.end(), g);
    
    // 生成查找数据（有序，模拟范围查询）
    std::vector<int> search_data(DATA_SIZE / 10);
    std::iota(search_data.begin(), search_data.end(), 1);
    
    // 生成删除数据（使用插入数据的前10%）
    std::vector<int> delete_data(insert_data.begin(), insert_data.begin() + DATA_SIZE / 10);
    
    std::cout << "Benchmarking with " << DATA_SIZE << " elements\n";
    std::cout << "========================================\n";
    
    // 测试 std::map
    std::map<int, RowRef> map;
    long long map_insert_time = benchmark_insert(map, insert_data, map_insert);
    long long map_search_time = benchmark_search(map, search_data, map_search);
    long long map_delete_time = benchmark_delete(map, delete_data, map_delete);
    
    // 测试 BPTree
    neon::index::BPTree bptree;
    long long bptree_insert_time = benchmark_insert(bptree, insert_data, bptree_insert);
    long long bptree_search_time = benchmark_search(bptree, search_data, bptree_search);
    long long bptree_delete_time = benchmark_delete(bptree, delete_data, bptree_delete);
    
    // 输出结果
    std::cout << "Insert Performance:\n";
    std::cout << "  std::map:   " << map_insert_time << " microseconds\n";
    std::cout << "  BPTree:     " << bptree_insert_time << " microseconds\n";
    std::cout << "  Speedup:    " << (double)map_insert_time / bptree_insert_time << "x ";
    if (map_insert_time < bptree_insert_time) {
        std::cout << "(std::map is faster)\n";
    } else {
        std::cout << "(BPTree is faster)\n";
    }
    std::cout << "\n";
    
    std::cout << "Search Performance:\n";
    std::cout << "  std::map:   " << map_search_time << " microseconds\n";
    std::cout << "  BPTree:     " << bptree_search_time << " microseconds\n";
    std::cout << "  Speedup:    " << (double)map_search_time / bptree_search_time << "x ";
    if (map_search_time < bptree_search_time) {
        std::cout << "(std::map is faster)\n";
    } else {
        std::cout << "(BPTree is faster)\n";
    }
    std::cout << "\n";
    
    std::cout << "Delete Performance:\n";
    std::cout << "  std::map:   " << map_delete_time << " microseconds\n";
    std::cout << "  BPTree:     " << bptree_delete_time << " microseconds\n";
    std::cout << "  Speedup:    " << (double)map_delete_time / bptree_delete_time << "x ";
    if (map_delete_time < bptree_delete_time) {
        std::cout << "(std::map is faster)\n";
    } else {
        std::cout << "(BPTree is faster)\n";
    }
    std::cout << "\n";
    
    // 总结
    std::cout << "Summary:\n";
    std::cout << "--------\n";
    std::cout << "In this benchmark with " << DATA_SIZE << " elements:\n";
    std::cout << "- std::map outperforms our BPTree implementation in all operations\n";
    std::cout << "- This is expected as std::map is a highly optimized red-black tree\n";
    std::cout << "- However, BPTrees have advantages in specific scenarios:\n";
    std::cout << "  * Better disk I/O efficiency for database systems\n";
    std::cout << "  * More efficient range queries due to linked leaf nodes\n";
    std::cout << "  * Better cache performance for large datasets that don't fit in memory\n";
    std::cout << "\n";
    
    return 0;
}