#include "bytecode/vm.h"
#include "parser/ast.h"
#include "src/storage/storage_engine.h"
#include "src/storage/table_heap.h"
#include <algorithm>
#include <variant>
#include <cmath>

namespace neon::bytecode {

namespace {

bool as_bool(const neon::type::Value& v) {
    switch (v.datatype()) {
        case neon::type::ValueType::Bool:
            return v.boolean();
        case neon::type::ValueType::Null:
            return false;
        case neon::type::ValueType::Int8:
            return v.i8() != 0;
        case neon::type::ValueType::Int16:
            return v.i16() != 0;
        case neon::type::ValueType::Int32:
            return v.i32() != 0;
        case neon::type::ValueType::Int64:
            return v.i64() != 0;
        case neon::type::ValueType::Uint8:
            return v.u8() != 0;
        case neon::type::ValueType::Uint16:
            return v.u16() != 0;
        case neon::type::ValueType::Uint32:
            return v.u32() != 0;
        case neon::type::ValueType::Uint64:
            return v.u64() != 0;
        case neon::type::ValueType::Float32:
            return v.f32() != 0.0f;
        case neon::type::ValueType::Float64:
            return v.f64() != 0.0;
        case neon::type::ValueType::String:
            return !v.string_ref().empty();
        case neon::type::ValueType::Char:
            return v.u8() != 0;
        default:
            return false;
    }
}

double to_double(const neon::type::Value& v) {
    switch (v.datatype()) {
        case neon::type::ValueType::Int8:  return static_cast<double>(v.i8());
        case neon::type::ValueType::Int16: return static_cast<double>(v.i16());
        case neon::type::ValueType::Int32: return static_cast<double>(v.i32());
        case neon::type::ValueType::Int64: return static_cast<double>(v.i64());
        case neon::type::ValueType::Uint8:  return static_cast<double>(v.u8());
        case neon::type::ValueType::Uint16: return static_cast<double>(v.u16());
        case neon::type::ValueType::Uint32: return static_cast<double>(v.u32());
        case neon::type::ValueType::Uint64: return static_cast<double>(v.u64());
        case neon::type::ValueType::Float32: return static_cast<double>(v.f32());
        case neon::type::ValueType::Float64: return v.f64();
        default:
            return 0.0;
    }
}

int64_t to_int64(const neon::type::Value& v) {
    switch (v.datatype()) {
        case neon::type::ValueType::Int8:  return static_cast<int64_t>(v.i8());
        case neon::type::ValueType::Int16: return static_cast<int64_t>(v.i16());
        case neon::type::ValueType::Int32: return static_cast<int64_t>(v.i32());
        case neon::type::ValueType::Int64: return v.i64();
        case neon::type::ValueType::Uint8:  return static_cast<int64_t>(v.u8());
        case neon::type::ValueType::Uint16: return static_cast<int64_t>(v.u16());
        case neon::type::ValueType::Uint32: return static_cast<int64_t>(v.u32());
        case neon::type::ValueType::Uint64: return static_cast<int64_t>(v.u64());
        case neon::type::ValueType::Float32: return static_cast<int64_t>(v.f32());
        case neon::type::ValueType::Float64: return static_cast<int64_t>(v.f64());
        default:
            return 0;
    }
}

bool is_floating(const neon::type::Value& v) {
    return v.datatype() == neon::type::ValueType::Float32 ||
           v.datatype() == neon::type::ValueType::Float64;
}

neon::type::Value make_bool_value(bool b) {
    return neon::type::Value(neon::type::ValueType::Bool, b);
}

neon::type::Value do_add(const neon::type::Value& l, const neon::type::Value& r) {
    if (is_floating(l) || is_floating(r)) {
        return neon::type::Value(neon::type::ValueType::Float64, to_double(l) + to_double(r));
    }
    return neon::type::Value(neon::type::ValueType::Int64, to_int64(l) + to_int64(r));
}

neon::type::Value do_sub(const neon::type::Value& l, const neon::type::Value& r) {
    if (is_floating(l) || is_floating(r)) {
        return neon::type::Value(neon::type::ValueType::Float64, to_double(l) - to_double(r));
    }
    return neon::type::Value(neon::type::ValueType::Int64, to_int64(l) - to_int64(r));
}

neon::type::Value do_mul(const neon::type::Value& l, const neon::type::Value& r) {
    if (is_floating(l) || is_floating(r)) {
        return neon::type::Value(neon::type::ValueType::Float64, to_double(l) * to_double(r));
    }
    return neon::type::Value(neon::type::ValueType::Int64, to_int64(l) * to_int64(r));
}

neon::type::Value do_div(const neon::type::Value& l, const neon::type::Value& r) {
    double rd = to_double(r);
    if (rd == 0.0) {
        // Return 0 on division by zero
        return neon::type::Value(neon::type::ValueType::Int64, static_cast<int64_t>(0));
    }
    if (is_floating(l) || is_floating(r)) {
        return neon::type::Value(neon::type::ValueType::Float64, to_double(l) / rd);
    }
    return neon::type::Value(neon::type::ValueType::Int64, to_int64(l) / to_int64(r));
}

neon::type::Value do_neg(const neon::type::Value& v) {
    if (is_floating(v)) {
        return neon::type::Value(neon::type::ValueType::Float64, -to_double(v));
    }
    return neon::type::Value(neon::type::ValueType::Int64, -to_int64(v));
}

bool do_le(const neon::type::Value& l, const neon::type::Value& r) {
    return !(r < l);
}

bool do_ge(const neon::type::Value& l, const neon::type::Value& r) {
    return !(l < r);
}

bool do_ne(const neon::type::Value& l, const neon::type::Value& r) {
    return !(l == r);
}

} // anonymous namespace

VirtualMachine::VirtualMachine(neon::catalog::SchemaManager& schema_mgr, neon::storage::StorageEngine& engine)
    : schema_mgr_(schema_mgr), engine_(engine) {}

void VirtualMachine::push(const neon::type::Value& val) {
    stack_.push_back(val);
}

neon::type::Value VirtualMachine::pop() {
    if (stack_.empty()) {
        return neon::type::Value();
    }
    neon::type::Value v = stack_.back();
    stack_.pop_back();
    return v;
}

const neon::type::Value& VirtualMachine::peek() const {
    static neon::type::Value null_val;
    if (stack_.empty()) {
        return null_val;
    }
    return stack_.back();
}

void VirtualMachine::reset() {
    stack_.clear();
    pc_ = 0;
    cursors_.clear();
    sorters_.clear();
    agg_states_.clear();
    pending_row_.reset();
    txn_ = nullptr;
    result_ = ExecuteResult();
}

ExecuteResult VirtualMachine::execute(const Program& program) {
    reset();
    result_.columns = program.result_columns;
    
    // Begin transaction for this execution
    txn_ = engine_.begin_txn();
    
    while (pc_ < static_cast<int32_t>(program.instructions.size())) {
        const auto& inst = program.instructions[pc_];
        
        switch (inst.op) {
            case OpCode::Const: {
                const auto& constant = program.constants[inst.operand1];
                if (std::holds_alternative<neon::type::Value>(constant)) {
                    push(std::get<neon::type::Value>(constant));
                } else {
                    result_.success = false;
                    result_.error = neon::base::Error(
                        static_cast<int32_t>(neon::base::InternalErr),
                        "Const opcode expects a Value constant");
                    if (txn_) { engine_.abort_txn(txn_); txn_ = nullptr; }
                    return result_;
                }
                pc_++;
                break;
            }
            
            case OpCode::Pop: {
                pop();
                pc_++;
                break;
            }
            
            case OpCode::Column: {
                auto it = cursors_.find(inst.operand1);
                if (it == cursors_.end() || !it->second.has_current) {
                    result_.success = false;
                    result_.error = neon::base::Error(
                        static_cast<int32_t>(neon::base::InternalErr),
                        "Column opcode: invalid cursor");
                    if (txn_) { engine_.abort_txn(txn_); txn_ = nullptr; }
                    return result_;
                }
                auto& cursor = it->second;
                auto& row = cursor.current_row_cache;
                if (inst.operand2 < 0 || inst.operand2 >= static_cast<int32_t>(row.values_.size())) {
                    result_.success = false;
                    result_.error = neon::base::Error(
                        static_cast<int32_t>(neon::base::InternalErr),
                        "Column opcode: column index out of bounds");
                    if (txn_) { engine_.abort_txn(txn_); txn_ = nullptr; }
                    return result_;
                }
                push(row.values_[inst.operand2]);
                pc_++;
                break;
            }
            
            case OpCode::Add: {
                auto r = pop();
                auto l = pop();
                push(do_add(l, r));
                pc_++;
                break;
            }
            case OpCode::Sub: {
                auto r = pop();
                auto l = pop();
                push(do_sub(l, r));
                pc_++;
                break;
            }
            case OpCode::Mul: {
                auto r = pop();
                auto l = pop();
                push(do_mul(l, r));
                pc_++;
                break;
            }
            case OpCode::Div: {
                auto r = pop();
                auto l = pop();
                push(do_div(l, r));
                pc_++;
                break;
            }
            case OpCode::Neg: {
                auto v = pop();
                push(do_neg(v));
                pc_++;
                break;
            }
            
            case OpCode::Eq: {
                auto r = pop();
                auto l = pop();
                push(make_bool_value(l == r));
                pc_++;
                break;
            }
            case OpCode::Ne: {
                auto r = pop();
                auto l = pop();
                push(make_bool_value(do_ne(l, r)));
                pc_++;
                break;
            }
            case OpCode::Lt: {
                auto r = pop();
                auto l = pop();
                push(make_bool_value(l < r));
                pc_++;
                break;
            }
            case OpCode::Le: {
                auto r = pop();
                auto l = pop();
                push(make_bool_value(do_le(l, r)));
                pc_++;
                break;
            }
            case OpCode::Gt: {
                auto r = pop();
                auto l = pop();
                push(make_bool_value(l > r));
                pc_++;
                break;
            }
            case OpCode::Ge: {
                auto r = pop();
                auto l = pop();
                push(make_bool_value(do_ge(l, r)));
                pc_++;
                break;
            }
            
            case OpCode::And: {
                auto r = pop();
                auto l = pop();
                push(make_bool_value(as_bool(l) && as_bool(r)));
                pc_++;
                break;
            }
            case OpCode::Or: {
                auto r = pop();
                auto l = pop();
                push(make_bool_value(as_bool(l) || as_bool(r)));
                pc_++;
                break;
            }
            case OpCode::Not: {
                auto v = pop();
                push(make_bool_value(!as_bool(v)));
                pc_++;
                break;
            }
            
            case OpCode::Jump: {
                pc_ = inst.operand1;
                break;
            }
            case OpCode::JumpIfFalse: {
                auto v = pop();
                if (!as_bool(v)) {
                    pc_ = inst.operand1;
                } else {
                    pc_++;
                }
                break;
            }
            case OpCode::JumpIfTrue: {
                auto v = pop();
                if (as_bool(v)) {
                    pc_ = inst.operand1;
                } else {
                    pc_++;
                }
                break;
            }
            case OpCode::Halt: {
                goto done;
            }
            
            case OpCode::OpenRead:
            case OpCode::OpenWrite: {
                int32_t cursor_id = inst.operand1;
                std::string table_name = std::get<std::string>(program.constants[inst.operand2]);
                Cursor c;
                c.table_name = table_name;
                c.is_write = (inst.op == OpCode::OpenWrite);
                
                // Get table heap from storage engine
                auto* heap = engine_.get_table_heap(table_name);
                if (!heap) {
                    if (inst.op == OpCode::OpenWrite) {
                        auto [found, schema] = schema_mgr_.query_table(table_name);
                        if (!found) {
                            result_.success = false;
                            result_.error = neon::base::Error(
                                static_cast<int32_t>(neon::base::TableNotFound),
                                "Table not found: " + table_name);
                            if (txn_) { engine_.abort_txn(txn_); txn_ = nullptr; }
                            return result_;
                        }
                    }
                    // For read on non-existent table, create empty cursor
                    c.heap = nullptr;
                } else {
                    c.heap = heap;
                    c.schema = heap->schema();
                    c.iter = std::make_unique<neon::storage::TableIterator>(
                        const_cast<neon::storage::TableHeap*>(heap), heap->schema());
                }
                cursors_[cursor_id] = std::move(c);
                pc_++;
                break;
            }
            
            case OpCode::Rewind: {
                auto it = cursors_.find(inst.operand1);
                if (it == cursors_.end() || !it->second.iter) {
                    pc_ = inst.operand2;
                    break;
                }
                auto& cursor = it->second;
                cursor.iter->rewind();
                if (!cursor.iter->valid()) {
                    pc_ = inst.operand2;
                } else {
                    cursor.current_rid = cursor.iter->current_row_id();
                    cursor.current_row_cache = cursor.iter->current_row();
                    cursor.has_current = true;
                    pc_++;
                }
                break;
            }
            
            case OpCode::Next: {
                auto it = cursors_.find(inst.operand1);
                if (it == cursors_.end() || !it->second.iter) {
                    pc_++;
                    break;
                }
                auto& cursor = it->second;
                cursor.iter->next();
                if (cursor.iter->valid()) {
                    cursor.current_rid = cursor.iter->current_row_id();
                    cursor.current_row_cache = cursor.iter->current_row();
                    cursor.has_current = true;
                    pc_ = inst.operand2;
                } else {
                    cursor.has_current = false;
                    pc_++;
                }
                break;
            }
            
            case OpCode::Close: {
                cursors_.erase(inst.operand1);
                pc_++;
                break;
            }
            
            case OpCode::ResultRow: {
                auto row = std::make_shared<neon::type::Row>();
                std::vector<neon::type::Value> vals(inst.operand1);
                for (int i = inst.operand1 - 1; i >= 0; i--) {
                    vals[i] = pop();
                }
                for (auto& v : vals) {
                    row->append_value(v);
                }
                result_.rows.push_back(row);
                pc_++;
                break;
            }
            
            case OpCode::ColumnName: {
                std::string name = std::get<std::string>(program.constants[inst.operand2]);
                if (static_cast<int32_t>(result_.columns.size()) <= inst.operand1) {
                    result_.columns.resize(inst.operand1 + 1);
                }
                result_.columns[inst.operand1] = name;
                pc_++;
                break;
            }
            
            case OpCode::MakeRow: {
                neon::type::Row row;
                int32_t num_cols = inst.operand1;
                std::vector<neon::type::Value> vals(num_cols);
                for (int i = num_cols - 1; i >= 0; i--) {
                    vals[i] = pop();
                }
                for (auto& v : vals) {
                    row.append_value(v);
                }
                pending_row_ = std::move(row);
                pc_++;
                break;
            }
            
            case OpCode::InsertRow: {
                auto it = cursors_.find(inst.operand1);
                if (it == cursors_.end() || !it->second.heap) {
                    result_.success = false;
                    result_.error = neon::base::Error(
                        static_cast<int32_t>(neon::base::InternalErr),
                        "InsertRow: invalid cursor");
                    if (txn_) { engine_.abort_txn(txn_); txn_ = nullptr; }
                    return result_;
                }
                if (!pending_row_.has_value()) {
                    result_.success = false;
                    result_.error = neon::base::Error(
                        static_cast<int32_t>(neon::base::InternalErr),
                        "InsertRow: no pending row");
                    if (txn_) { engine_.abort_txn(txn_); txn_ = nullptr; }
                    return result_;
                }
                auto rid = it->second.heap->insert_row(txn_, pending_row_.value());
                if (!rid.is_valid()) {
                    result_.success = false;
                    result_.error = neon::base::Error(
                        static_cast<int32_t>(neon::base::InternalErr),
                        "InsertRow: insert failed");
                    if (txn_) { engine_.abort_txn(txn_); txn_ = nullptr; }
                    return result_;
                }
                pending_row_.reset();
                result_.rows_affected++;
                pc_++;
                break;
            }
            
            case OpCode::DeleteRow: {
                auto it = cursors_.find(inst.operand1);
                if (it == cursors_.end() || !it->second.heap || !it->second.has_current) {
                    result_.success = false;
                    result_.error = neon::base::Error(
                        static_cast<int32_t>(neon::base::InternalErr),
                        "DeleteRow: invalid cursor");
                    if (txn_) { engine_.abort_txn(txn_); txn_ = nullptr; }
                    return result_;
                }
                auto& cursor = it->second;
                cursor.heap->delete_row(txn_, cursor.current_rid);
                result_.rows_affected++;
                pc_++;
                break;
            }
            
            case OpCode::UpdateRow: {
                auto it = cursors_.find(inst.operand1);
                if (it == cursors_.end() || !it->second.heap || !it->second.has_current) {
                    result_.success = false;
                    result_.error = neon::base::Error(
                        static_cast<int32_t>(neon::base::InternalErr),
                        "UpdateRow: invalid cursor");
                    if (txn_) { engine_.abort_txn(txn_); txn_ = nullptr; }
                    return result_;
                }
                auto& cursor = it->second;
                neon::type::Row updated_row = cursor.current_row_cache;
                int32_t num_assigns = inst.operand2;
                std::vector<std::pair<int32_t, neon::type::Value>> updates;
                for (int i = 0; i < num_assigns; i++) {
                    neon::type::Value value = pop();
                    neon::type::Value col_idx_val = pop();
                    int32_t col_idx = col_idx_val.i32();
                    updates.push_back({col_idx, value});
                }
                for (auto& [col_idx, value] : updates) {
                    if (col_idx >= 0 && col_idx < static_cast<int32_t>(updated_row.values_.size())) {
                        updated_row.values_[col_idx] = value;
                    }
                }
                cursor.heap->update_row(txn_, cursor.current_rid, updated_row);
                result_.rows_affected++;
                pc_++;
                break;
            }
            
            case OpCode::AggInit: {
                agg_states_.clear();
                agg_states_.resize(inst.operand1);
                pc_++;
                break;
            }
            
            case OpCode::AggStep: {
                int32_t agg_idx = inst.operand1;
                int32_t func_type = inst.operand2;
                if (agg_idx < 0 || agg_idx >= static_cast<int32_t>(agg_states_.size())) {
                    result_.success = false;
                    result_.error = neon::base::Error(
                        static_cast<int32_t>(neon::base::InternalErr),
                        "AggStep: invalid agg index");
                    if (txn_) { engine_.abort_txn(txn_); txn_ = nullptr; }
                    return result_;
                }
                auto& state = agg_states_[agg_idx];
                state.func_type = func_type;
                neon::type::Value val = pop();
                
                switch (func_type) {
                    case 0: { // COUNT
                        state.count++;
                        break;
                    }
                    case 1: { // SUM
                        if (!state.initialized) {
                            state.result = val;
                            state.initialized = true;
                        } else {
                            state.result = do_add(state.result, val);
                        }
                        break;
                    }
                    case 2: { // AVG
                        if (!state.initialized) {
                            state.result = val;
                            state.initialized = true;
                        } else {
                            state.result = do_add(state.result, val);
                        }
                        state.count++;
                        break;
                    }
                    case 3: { // MIN
                        if (!state.initialized) {
                            state.result = val;
                            state.initialized = true;
                        } else if (val < state.result) {
                            state.result = val;
                        }
                        break;
                    }
                    case 4: { // MAX
                        if (!state.initialized) {
                            state.result = val;
                            state.initialized = true;
                        } else if (val > state.result) {
                            state.result = val;
                        }
                        break;
                    }
                }
                pc_++;
                break;
            }
            
            case OpCode::AggResult: {
                int32_t agg_idx = inst.operand1;
                if (agg_idx < 0 || agg_idx >= static_cast<int32_t>(agg_states_.size())) {
                    result_.success = false;
                    result_.error = neon::base::Error(
                        static_cast<int32_t>(neon::base::InternalErr),
                        "AggResult: invalid agg index");
                    if (txn_) { engine_.abort_txn(txn_); txn_ = nullptr; }
                    return result_;
                }
                auto& state = agg_states_[agg_idx];
                if (state.func_type == 0) { // COUNT
                    push(neon::type::Value(neon::type::ValueType::Int64, state.count));
                } else if (state.func_type == 2) { // AVG
                    if (state.count > 0) {
                        push(neon::type::Value(neon::type::ValueType::Float64,
                                               to_double(state.result) / static_cast<double>(state.count)));
                    } else {
                        push(neon::type::Value(neon::type::ValueType::Int64, static_cast<int64_t>(0)));
                    }
                } else {
                    push(state.result);
                }
                pc_++;
                break;
            }
            
            case OpCode::AggReset: {
                for (auto& state : agg_states_) {
                    state.count = 0;
                    state.initialized = false;
                    state.result = neon::type::Value();
                }
                pc_++;
                break;
            }
            
            case OpCode::SorterOpen: {
                int32_t sorter_id = inst.operand1;
                Sorter s;
                s.sort_keys.resize(inst.operand2);
                sorters_[sorter_id] = std::move(s);
                pc_++;
                break;
            }
            
            case OpCode::SortKeyDef: {
                auto it = sorters_.find(inst.operand1);
                if (it != sorters_.end()) {
                    if (inst.operand2 >= 0 && inst.operand2 < static_cast<int32_t>(it->second.sort_keys.size())) {
                        it->second.sort_keys[inst.operand2].column_index = inst.operand2;
                        it->second.sort_keys[inst.operand2].ascending = (inst.operand3 != 0);
                    }
                }
                pc_++;
                break;
            }
            
            case OpCode::SorterInsert: {
                auto it = sorters_.find(inst.operand1);
                if (it == sorters_.end()) {
                    result_.success = false;
                    result_.error = neon::base::Error(
                        static_cast<int32_t>(neon::base::InternalErr),
                        "SorterInsert: invalid sorter");
                    if (txn_) { engine_.abort_txn(txn_); txn_ = nullptr; }
                    return result_;
                }
                int32_t num_vals = inst.operand2;
                auto row = std::make_shared<neon::type::Row>();
                std::vector<neon::type::Value> vals(num_vals);
                for (int i = num_vals - 1; i >= 0; i--) {
                    vals[i] = pop();
                }
                for (auto& v : vals) {
                    row->append_value(v);
                }
                it->second.rows.push_back(row);
                pc_++;
                break;
            }
            
            case OpCode::SorterSort: {
                auto it = sorters_.find(inst.operand1);
                if (it != sorters_.end()) {
                    auto& sorter = it->second;
                    std::sort(sorter.rows.begin(), sorter.rows.end(),
                        [&sorter](const auto& a, const auto& b) {
                            for (const auto& key : sorter.sort_keys) {
                                int32_t idx = key.column_index;
                                if (idx < 0 || idx >= static_cast<int32_t>(a->values_.size()) ||
                                    idx >= static_cast<int32_t>(b->values_.size())) {
                                    continue;
                                }
                                const auto& av = a->values_[idx];
                                const auto& bv = b->values_[idx];
                                if (av == bv) continue;
                                if (key.ascending) {
                                    return av < bv;
                                } else {
                                    return bv < av;
                                }
                            }
                            return false;
                        });
                }
                pc_++;
                break;
            }
            
            case OpCode::SorterRewind: {
                auto it = sorters_.find(inst.operand1);
                if (it == sorters_.end() || it->second.rows.empty()) {
                    pc_ = inst.operand2;
                } else {
                    it->second.current_row = 0;
                    pc_++;
                }
                break;
            }
            
            case OpCode::SorterNext: {
                auto it = sorters_.find(inst.operand1);
                if (it == sorters_.end()) {
                    pc_++;
                    break;
                }
                auto& sorter = it->second;
                sorter.current_row++;
                if (sorter.current_row < static_cast<int32_t>(sorter.rows.size())) {
                    pc_ = inst.operand2;
                } else {
                    pc_++;
                }
                break;
            }
            
            case OpCode::SorterColumn: {
                auto it = sorters_.find(inst.operand1);
                if (it == sorters_.end()) {
                    result_.success = false;
                    result_.error = neon::base::Error(
                        static_cast<int32_t>(neon::base::InternalErr),
                        "SorterColumn: invalid sorter");
                    if (txn_) { engine_.abort_txn(txn_); txn_ = nullptr; }
                    return result_;
                }
                auto& sorter = it->second;
                if (sorter.current_row < 0 ||
                    sorter.current_row >= static_cast<int32_t>(sorter.rows.size())) {
                    result_.success = false;
                    result_.error = neon::base::Error(
                        static_cast<int32_t>(neon::base::InternalErr),
                        "SorterColumn: sorter out of bounds");
                    if (txn_) { engine_.abort_txn(txn_); txn_ = nullptr; }
                    return result_;
                }
                auto& row = *sorter.rows[sorter.current_row];
                if (inst.operand2 < 0 || inst.operand2 >= static_cast<int32_t>(row.values_.size())) {
                    result_.success = false;
                    result_.error = neon::base::Error(
                        static_cast<int32_t>(neon::base::InternalErr),
                        "SorterColumn: column index out of bounds");
                    if (txn_) { engine_.abort_txn(txn_); txn_ = nullptr; }
                    return result_;
                }
                push(row.values_[inst.operand2]);
                pc_++;
                break;
            }
            
            case OpCode::CreateTable: {
                std::string table_name = std::get<std::string>(program.constants[inst.operand1]);
                auto col_defs = std::get<std::vector<neon::parser::ColumnDef>>(program.constants[inst.operand2]);
                std::vector<neon::catalog::Column> columns;
                for (const auto& def : col_defs) {
                    neon::catalog::Column col{};
                    col.column_name_ = def.name;
                    col.data_type_ = def.data_type;
                    col.type_len_ = def.type_len;
                    col.is_primary_key_ = def.is_primary_key;
                    col.is_nullable_ = def.is_primary_key ? false : def.is_nullable;
                    col.is_unique_ = def.is_unique;
                    if (def.default_value.has_value()) {
                        col.default_value_ = def.default_value.value();
                    }
                    columns.push_back(col);
                }
                auto [create_ok, schema_id] = schema_mgr_.create_table(table_name, columns);
                if (!create_ok) {
                    result_.success = false;
                    result_.error = neon::base::Error(
                        static_cast<int32_t>(neon::base::InternalErr),
                        "Failed to create table: " + table_name);
                    if (txn_) { engine_.abort_txn(txn_); txn_ = nullptr; }
                    return result_;
                }
                // Create storage for the table
                auto [found_schema, schema_ref] = schema_mgr_.query_table(table_name);
                if (found_schema) {
                    engine_.create_table(table_name, *schema_ref);
                }
                result_.message = "Table created";
                pc_++;
                break;
            }
            
            case OpCode::DropTable: {
                std::string table_name = std::get<std::string>(program.constants[inst.operand1]);
                bool if_exists = (inst.operand2 != 0);
                auto [found, schema] = schema_mgr_.query_table(table_name);
                if (!found) {
                    if (if_exists) {
                        result_.message = "Table does not exist";
                        pc_++;
                        break;
                    }
                    result_.success = false;
                    result_.error = neon::base::Error(
                        static_cast<int32_t>(neon::base::TableNotFound),
                        "Table not found: " + table_name);
                    if (txn_) { engine_.abort_txn(txn_); txn_ = nullptr; }
                    return result_;
                }
                auto [success, error] = schema_mgr_.delete_table(table_name);
                if (!success) {
                    result_.success = false;
                    result_.error = error;
                    if (txn_) { engine_.abort_txn(txn_); txn_ = nullptr; }
                    return result_;
                }
                engine_.drop_table(table_name);
                result_.message = "Table dropped";
                pc_++;
                break;
            }
            
            case OpCode::Dup: {
                if (!stack_.empty()) {
                    push(stack_.back());
                }
                pc_++;
                break;
            }
            
            case OpCode::Noop: {
                pc_++;
                break;
            }
            
            default: {
                pc_++;
                break;
            }
        }
    }
    
done:
    // Commit on success
    if (txn_) {
        engine_.commit_txn(txn_);
        txn_ = nullptr;
    }
    result_.success = true;
    if (!result_.rows.empty()) {
        result_.message = std::to_string(result_.rows.size()) + " rows in set";
    } else if (result_.rows_affected > 0) {
        result_.message = std::to_string(result_.rows_affected) + " rows affected";
    } else if (result_.message.empty()) {
        result_.message = "OK";
    }
    return result_;
}

} // namespace neon::bytecode
