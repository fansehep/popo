#include "bytecode/vm.h"
#include <algorithm>
#include <cmath>
#include <variant>
#include "parser/ast.h"
#include "src/storage/row_serializer.h"
#include "src/storage/storage_engine.h"
#include "src/storage/table_heap.h"

namespace popo::bytecode
{

namespace
{

bool as_bool(const popo::type::Value & v)
{
    switch (v.datatype())
    {
        case popo::type::ValueType::Bool:
            return v.boolean();
        case popo::type::ValueType::Null:
            return false;
        case popo::type::ValueType::Int8:
            return v.i8() != 0;
        case popo::type::ValueType::Int16:
            return v.i16() != 0;
        case popo::type::ValueType::Int32:
            return v.i32() != 0;
        case popo::type::ValueType::Int64:
            return v.i64() != 0;
        case popo::type::ValueType::Uint8:
            return v.u8() != 0;
        case popo::type::ValueType::Uint16:
            return v.u16() != 0;
        case popo::type::ValueType::Uint32:
            return v.u32() != 0;
        case popo::type::ValueType::Uint64:
            return v.u64() != 0;
        case popo::type::ValueType::Float32:
            return v.f32() != 0.0f;
        case popo::type::ValueType::Float64:
            return v.f64() != 0.0;
        case popo::type::ValueType::String:
            return !v.string_ref().empty();
        case popo::type::ValueType::Char:
            return v.u8() != 0;
        default:
            return false;
    }
}

double to_double(const popo::type::Value & v)
{
    switch (v.datatype())
    {
        case popo::type::ValueType::Int8:
            return static_cast<double>(v.i8());
        case popo::type::ValueType::Int16:
            return static_cast<double>(v.i16());
        case popo::type::ValueType::Int32:
            return static_cast<double>(v.i32());
        case popo::type::ValueType::Int64:
            return static_cast<double>(v.i64());
        case popo::type::ValueType::Uint8:
            return static_cast<double>(v.u8());
        case popo::type::ValueType::Uint16:
            return static_cast<double>(v.u16());
        case popo::type::ValueType::Uint32:
            return static_cast<double>(v.u32());
        case popo::type::ValueType::Uint64:
            return static_cast<double>(v.u64());
        case popo::type::ValueType::Float32:
            return static_cast<double>(v.f32());
        case popo::type::ValueType::Float64:
            return v.f64();
        default:
            return 0.0;
    }
}

int64_t to_int64(const popo::type::Value & v)
{
    switch (v.datatype())
    {
        case popo::type::ValueType::Int8:
            return static_cast<int64_t>(v.i8());
        case popo::type::ValueType::Int16:
            return static_cast<int64_t>(v.i16());
        case popo::type::ValueType::Int32:
            return static_cast<int64_t>(v.i32());
        case popo::type::ValueType::Int64:
            return v.i64();
        case popo::type::ValueType::Uint8:
            return static_cast<int64_t>(v.u8());
        case popo::type::ValueType::Uint16:
            return static_cast<int64_t>(v.u16());
        case popo::type::ValueType::Uint32:
            return static_cast<int64_t>(v.u32());
        case popo::type::ValueType::Uint64:
            return static_cast<int64_t>(v.u64());
        case popo::type::ValueType::Float32:
            return static_cast<int64_t>(v.f32());
        case popo::type::ValueType::Float64:
            return static_cast<int64_t>(v.f64());
        default:
            return 0;
    }
}

bool is_floating(const popo::type::Value & v)
{
    return v.datatype() == popo::type::ValueType::Float32 || v.datatype() == popo::type::ValueType::Float64;
}

popo::type::Value make_bool_value(bool b)
{
    return popo::type::Value(popo::type::ValueType::Bool, b);
}

popo::type::Value do_add(const popo::type::Value & l, const popo::type::Value & r)
{
    if (is_floating(l) || is_floating(r))
    {
        return popo::type::Value(popo::type::ValueType::Float64, to_double(l) + to_double(r));
    }
    return popo::type::Value(popo::type::ValueType::Int64, to_int64(l) + to_int64(r));
}

popo::type::Value do_sub(const popo::type::Value & l, const popo::type::Value & r)
{
    if (is_floating(l) || is_floating(r))
    {
        return popo::type::Value(popo::type::ValueType::Float64, to_double(l) - to_double(r));
    }
    return popo::type::Value(popo::type::ValueType::Int64, to_int64(l) - to_int64(r));
}

popo::type::Value do_mul(const popo::type::Value & l, const popo::type::Value & r)
{
    if (is_floating(l) || is_floating(r))
    {
        return popo::type::Value(popo::type::ValueType::Float64, to_double(l) * to_double(r));
    }
    return popo::type::Value(popo::type::ValueType::Int64, to_int64(l) * to_int64(r));
}

popo::type::Value do_div(const popo::type::Value & l, const popo::type::Value & r)
{
    double rd = to_double(r);
    if (rd == 0.0)
    {
        // Return 0 on division by zero
        return popo::type::Value(popo::type::ValueType::Int64, static_cast<int64_t>(0));
    }
    if (is_floating(l) || is_floating(r))
    {
        return popo::type::Value(popo::type::ValueType::Float64, to_double(l) / rd);
    }
    return popo::type::Value(popo::type::ValueType::Int64, to_int64(l) / to_int64(r));
}

popo::type::Value do_neg(const popo::type::Value & v)
{
    if (is_floating(v))
    {
        return popo::type::Value(popo::type::ValueType::Float64, -to_double(v));
    }
    return popo::type::Value(popo::type::ValueType::Int64, -to_int64(v));
}

bool do_le(const popo::type::Value & l, const popo::type::Value & r)
{
    return !(r < l);
}

bool do_ge(const popo::type::Value & l, const popo::type::Value & r)
{
    return !(l < r);
}

bool do_ne(const popo::type::Value & l, const popo::type::Value & r)
{
    return !(l == r);
}

} // anonymous namespace

VirtualMachine::VirtualMachine(popo::catalog::SchemaManager & schema_mgr, popo::storage::StorageEngine & engine)
    : schema_mgr_(schema_mgr), engine_(engine)
{
}

void VirtualMachine::push(const popo::type::Value & val)
{
    stack_.push_back(val);
}

popo::type::Value VirtualMachine::pop()
{
    if (stack_.empty())
    {
        return popo::type::Value();
    }
    popo::type::Value v = stack_.back();
    stack_.pop_back();
    return v;
}

const popo::type::Value & VirtualMachine::peek() const
{
    static popo::type::Value null_val;
    if (stack_.empty())
    {
        return null_val;
    }
    return stack_.back();
}

void VirtualMachine::reset()
{
    stack_.clear();
    pc_ = 0;
    cursors_.clear();
    sorters_.clear();
    agg_states_.clear();
    pending_row_.reset();
    txn_ = nullptr;
    result_ = ExecuteSuccess();
}

popo::base::Result<ExecuteSuccess> VirtualMachine::execute(const Program & program, popo::txn::Transaction * outer_txn)
{
    reset();
    own_txn_ = outer_txn == nullptr;
    result_.columns = program.result_columns;

    txn_ = own_txn_ ? engine_.begin_txn() : outer_txn;

    while (pc_ < static_cast<int32_t>(program.instructions.size()))
    {
        const auto & inst = program.instructions[pc_];

        switch (inst.op)
        {
            case OpCode::Const: {
                const auto & constant = program.constants[inst.operand1];
                if (std::holds_alternative<popo::type::Value>(constant))
                {
                    push(std::get<popo::type::Value>(constant));
                }
                else
                {
                    return error(popo::base::Error(static_cast<int32_t>(popo::base::InternalErr), "Const opcode expects a Value constant"));
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
                if (it == cursors_.end() || !it->second.has_current)
                {
                    return error(popo::base::Error(static_cast<int32_t>(popo::base::InternalErr), "Column opcode: invalid cursor"));
                }
                auto & cursor = it->second;
                if (!cursor.row_materialized && !cursor.is_write)
                {
                    if (cursor.iter && cursor.iter->valid())
                    {
                        auto [data, len] = cursor.iter->current_bytes();
                        push(popo::storage::RowSerializer::read_column(data, len, cursor.schema, inst.operand2));
                        pc_++;
                        break;
                    }
                    if (cursor.heap && cursor.has_current)
                    {
                        auto val = cursor.heap->read_column(cursor.current_rid, inst.operand2);
                        if (!val.has_value())
                        {
                            return error(popo::base::Error(static_cast<int32_t>(popo::base::InternalErr), "Column opcode: row missing"));
                        }
                        push(*val);
                        pc_++;
                        break;
                    }
                }
                if (!cursor.row_materialized)
                {
                    if (cursor.iter && cursor.iter->valid())
                    {
                        cursor.current_row_cache = cursor.iter->current_row();
                    }
                    else if (cursor.heap)
                    {
                        auto tuple_opt = cursor.heap->get_row(cursor.current_rid);
                        if (!tuple_opt.has_value())
                        {
                            return error(popo::base::Error(static_cast<int32_t>(popo::base::InternalErr), "Column opcode: row missing"));
                        }
                        cursor.current_row_cache = tuple_opt->deform(cursor.schema);
                    }
                    cursor.row_materialized = true;
                }
                auto & row = cursor.current_row_cache;
                if (inst.operand2 < 0 || inst.operand2 >= static_cast<int32_t>(row.values_.size()))
                {
                    return error(
                        popo::base::Error(static_cast<int32_t>(popo::base::InternalErr), "Column opcode: column index out of bounds"));
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
                if (!as_bool(v))
                {
                    pc_ = inst.operand1;
                }
                else
                {
                    pc_++;
                }
                break;
            }
            case OpCode::JumpIfTrue: {
                auto v = pop();
                if (as_bool(v))
                {
                    pc_ = inst.operand1;
                }
                else
                {
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
                auto * heap = engine_.get_table_heap(table_name);
                if (!heap)
                {
                    if (inst.op == OpCode::OpenWrite)
                    {
                        auto qr = schema_mgr_.query_table(table_name);
                        if (!qr)
                        {
                            return error(
                                popo::base::Error(static_cast<int32_t>(popo::base::TableNotFound), "Table not found: " + table_name));
                        }
                    }
                    // For read on non-existent table, create empty cursor
                    c.heap = nullptr;
                }
                else
                {
                    c.heap = heap;
                    c.schema = heap->schema();
                    c.iter = std::make_unique<popo::storage::TableIterator>(const_cast<popo::storage::TableHeap *>(heap), heap->schema());
                }
                cursors_[cursor_id] = std::move(c);
                pc_++;
                break;
            }

            case OpCode::Rewind: {
                auto it = cursors_.find(inst.operand1);
                if (it == cursors_.end() || !it->second.iter)
                {
                    pc_ = inst.operand2;
                    break;
                }
                auto & cursor = it->second;
                cursor.iter->rewind();
                if (!cursor.iter->valid())
                {
                    pc_ = inst.operand2;
                }
                else
                {
                    cursor.current_rid = cursor.iter->current_row_id();
                    cursor.row_materialized = false;
                    cursor.has_current = true;
                    pc_++;
                }
                break;
            }

            case OpCode::Next: {
                auto it = cursors_.find(inst.operand1);
                if (it == cursors_.end() || !it->second.iter)
                {
                    pc_++;
                    break;
                }
                auto & cursor = it->second;
                if (cursor.index_scan)
                {
                    cursor.index_pos++;
                    if (cursor.index_pos < cursor.index_hits.size() && cursor.heap)
                    {
                        cursor.current_rid = cursor.index_hits[cursor.index_pos];
                        auto tuple_opt = cursor.heap->get_row(cursor.current_rid);
                        if (!tuple_opt)
                        {
                            cursor.has_current = false;
                            cursor.index_scan = false;
                            pc_++;
                            break;
                        }
                        cursor.current_row_cache = tuple_opt->deform(cursor.schema);
                        cursor.row_materialized = true;
                        cursor.has_current = true;
                        pc_ = inst.operand2;
                    }
                    else
                    {
                        cursor.has_current = false;
                        cursor.index_scan = false;
                        pc_++;
                    }
                    break;
                }
                cursor.iter->next();
                if (cursor.iter->valid())
                {
                    cursor.current_rid = cursor.iter->current_row_id();
                    cursor.row_materialized = false;
                    cursor.has_current = true;
                    pc_ = inst.operand2;
                }
                else
                {
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
                auto row = std::make_shared<popo::type::Row>();
                std::vector<popo::type::Value> vals(inst.operand1);
                for (int i = inst.operand1 - 1; i >= 0; i--)
                {
                    vals[i] = pop();
                }
                for (auto & v : vals)
                {
                    row->append_value(v);
                }
                result_.rows.push_back(row);
                pc_++;
                break;
            }

            case OpCode::ColumnName: {
                std::string name = std::get<std::string>(program.constants[inst.operand2]);
                if (static_cast<int32_t>(result_.columns.size()) <= inst.operand1)
                {
                    result_.columns.resize(inst.operand1 + 1);
                }
                result_.columns[inst.operand1] = name;
                pc_++;
                break;
            }

            case OpCode::MakeRow: {
                popo::type::Row row;
                int32_t num_cols = inst.operand1;
                row.reserve_values(num_cols);
                std::vector<popo::type::Value> vals(num_cols);
                for (int i = num_cols - 1; i >= 0; i--)
                {
                    vals[i] = pop();
                }
                for (auto & v : vals)
                {
                    row.append_value(std::move(v));
                }
                pending_row_ = std::move(row);
                pc_++;
                break;
            }

            case OpCode::InsertRow: {
                auto it = cursors_.find(inst.operand1);
                if (it == cursors_.end() || !it->second.heap)
                {
                    return error(popo::base::Error(static_cast<int32_t>(popo::base::InternalErr), "InsertRow: invalid cursor"));
                }
                if (!pending_row_.has_value())
                {
                    return error(popo::base::Error(static_cast<int32_t>(popo::base::InternalErr), "InsertRow: no pending row"));
                }
                auto tuple = popo::storage::Tuple::form(it->second.schema, pending_row_.value());
                auto rid = it->second.heap->insert_row(txn_, tuple);
                if (!rid.is_valid())
                {
                    return error(popo::base::Error(static_cast<int32_t>(popo::base::InternalErr), "InsertRow: insert failed"));
                }
                pending_row_.reset();
                result_.rows_affected++;
                pc_++;
                break;
            }

            case OpCode::DeleteRow: {
                auto it = cursors_.find(inst.operand1);
                if (it == cursors_.end() || !it->second.heap || !it->second.has_current)
                {
                    return error(popo::base::Error(static_cast<int32_t>(popo::base::InternalErr), "DeleteRow: invalid cursor"));
                }
                auto & cursor = it->second;
                cursor.heap->delete_row(txn_, cursor.current_rid);
                result_.rows_affected++;
                pc_++;
                break;
            }

            case OpCode::UpdateRow: {
                auto it = cursors_.find(inst.operand1);
                if (it == cursors_.end() || !it->second.heap || !it->second.has_current)
                {
                    return error(popo::base::Error(static_cast<int32_t>(popo::base::InternalErr), "UpdateRow: invalid cursor"));
                }
                auto & cursor = it->second;
                popo::type::Row updated_row = cursor.current_row_cache;
                int32_t num_assigns = inst.operand2;
                std::vector<std::pair<int32_t, popo::type::Value>> updates;
                for (int i = 0; i < num_assigns; i++)
                {
                    popo::type::Value value = pop();
                    popo::type::Value col_idx_val = pop();
                    int32_t col_idx = col_idx_val.i32();
                    updates.push_back({col_idx, value});
                }
                for (auto & [col_idx, value] : updates)
                {
                    if (col_idx >= 0 && col_idx < static_cast<int32_t>(updated_row.values_.size()))
                    {
                        updated_row.values_[col_idx] = value;
                    }
                }
                auto new_tuple = popo::storage::Tuple::form(cursor.schema, updated_row);
                auto new_rid = cursor.heap->update_row(txn_, cursor.current_rid, new_tuple);
                result_.rows_affected++;
                cursor.current_rid = new_rid;
                pc_++;
                break;
            }

            case OpCode::SeekGe: {
                auto key = pop();
                auto it = cursors_.find(inst.operand1);
                if (it == cursors_.end() || !it->second.iter)
                {
                    pc_ = inst.operand2;
                    break;
                }
                auto & cursor = it->second;
                cursor.index_scan = false;
                cursor.iter->seek_ge(key);
                if (!cursor.iter->valid())
                {
                    cursor.has_current = false;
                    pc_ = inst.operand2;
                }
                else
                {
                    cursor.current_rid = cursor.iter->current_row_id();
                    cursor.row_materialized = false;
                    cursor.has_current = true;
                    pc_++;
                }
                break;
            }

            case OpCode::SeekIndex: {
                auto key = pop();
                auto it = cursors_.find(inst.operand1);
                if (it == cursors_.end() || !it->second.heap)
                {
                    pc_ = inst.operand2;
                    break;
                }
                auto & cursor = it->second;
                auto pks = cursor.heap->lookup_index(static_cast<size_t>(inst.operand3), key);
                cursor.index_hits.clear();
                cursor.index_hits.reserve(pks.size());
                for (const auto & pk : pks)
                {
                    auto rid = cursor.heap->lookup_pk(pk);
                    if (rid)
                    {
                        cursor.index_hits.push_back(*rid);
                    }
                }
                if (cursor.index_hits.empty())
                {
                    cursor.index_scan = false;
                    cursor.has_current = false;
                    pc_ = inst.operand2;
                    break;
                }
                cursor.index_scan = true;
                cursor.index_pos = 0;
                cursor.current_rid = cursor.index_hits[0];
                auto tuple_opt = cursor.heap->get_row(cursor.current_rid);
                if (!tuple_opt)
                {
                    cursor.has_current = false;
                    cursor.index_scan = false;
                    pc_ = inst.operand2;
                    break;
                }
                cursor.current_row_cache = tuple_opt->deform(cursor.schema);
                cursor.row_materialized = true;
                cursor.has_current = true;
                pc_++;
                break;
            }

            case OpCode::CreateIndex: {
                bool unique = inst.operand1 < 0;
                int32_t name_idx = unique ? -inst.operand1 - 1 : inst.operand1;
                std::string index_name = std::get<std::string>(program.constants[name_idx]);
                std::string table_name = std::get<std::string>(program.constants[inst.operand2]);
                std::string column_name = std::get<std::string>(program.constants[inst.operand3]);
                auto qr = schema_mgr_.query_table(table_name);
                if (!qr || !*qr)
                {
                    return error(popo::base::Error(static_cast<int32_t>(popo::base::TableNotFound), "Table not found: " + table_name));
                }
                int32_t col = -1;
                const auto & columns = (*qr)->columns_;
                for (size_t i = 0; i < columns.size(); ++i)
                {
                    if (columns[i].column_name_ == column_name)
                    {
                        col = static_cast<int32_t>(i);
                        break;
                    }
                }
                if (col < 0)
                {
                    return error(popo::base::Error(static_cast<int32_t>(popo::base::InternalErr), "Column not found: " + column_name));
                }
                auto created = schema_mgr_.create_index(table_name, index_name, {col}, unique);
                if (!created)
                {
                    return error(created.error());
                }
                try
                {
                    engine_.create_index(table_name, index_name, col, unique);
                }
                catch (const std::exception & ex)
                {
                    return error(popo::base::Error(static_cast<int32_t>(popo::base::InternalErr), ex.what()));
                }
                result_.message = "Index created";
                pc_++;
                break;
            }

            case OpCode::SeekByPk: {
                auto pk_val = pop();
                auto it = cursors_.find(inst.operand1);
                if (it == cursors_.end() || !it->second.heap)
                {
                    return error(popo::base::Error(static_cast<int32_t>(popo::base::InternalErr), "SeekByPk: invalid cursor"));
                }
                auto & cursor = it->second;
                auto rid_opt = cursor.heap->lookup_pk(pk_val);
                if (!rid_opt.has_value())
                {
                    // Row not found — jump to skip target (Close/Halt)
                    pc_ = inst.operand2;
                    cursor.has_current = false;
                    break;
                }
                cursor.current_rid = rid_opt.value();
                cursor.has_current = true;
                cursor.row_materialized = false;
                if (cursor.is_write)
                {
                    auto tuple_opt = cursor.heap->get_row(cursor.current_rid);
                    if (!tuple_opt.has_value())
                    {
                        cursor.has_current = false;
                        pc_ = inst.operand2;
                        break;
                    }
                    cursor.current_row_cache = tuple_opt->deform(cursor.schema);
                    cursor.row_materialized = true;
                }
                pc_++;
                break;
            }

            case OpCode::TableCount: {
                auto it = cursors_.find(inst.operand1);
                int64_t n = 0;
                if (it != cursors_.end() && it->second.heap)
                {
                    n = it->second.heap->count_rows();
                }
                push(popo::type::Value(popo::type::ValueType::Int64, n));
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
                if (agg_idx < 0 || agg_idx >= static_cast<int32_t>(agg_states_.size()))
                {
                    return error(popo::base::Error(static_cast<int32_t>(popo::base::InternalErr), "AggStep: invalid agg index"));
                }
                auto & state = agg_states_[agg_idx];
                state.func_type = func_type;
                popo::type::Value val = pop();

                switch (func_type)
                {
                    case 0: { // COUNT
                        state.count++;
                        break;
                    }
                    case 1: { // SUM
                        if (!state.initialized)
                        {
                            state.result = val;
                            state.initialized = true;
                        }
                        else
                        {
                            state.result = do_add(state.result, val);
                        }
                        break;
                    }
                    case 2: { // AVG
                        if (!state.initialized)
                        {
                            state.result = val;
                            state.initialized = true;
                        }
                        else
                        {
                            state.result = do_add(state.result, val);
                        }
                        state.count++;
                        break;
                    }
                    case 3: { // MIN
                        if (!state.initialized)
                        {
                            state.result = val;
                            state.initialized = true;
                        }
                        else if (val < state.result)
                        {
                            state.result = val;
                        }
                        break;
                    }
                    case 4: { // MAX
                        if (!state.initialized)
                        {
                            state.result = val;
                            state.initialized = true;
                        }
                        else if (val > state.result)
                        {
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
                if (agg_idx < 0 || agg_idx >= static_cast<int32_t>(agg_states_.size()))
                {
                    return error(popo::base::Error(static_cast<int32_t>(popo::base::InternalErr), "AggResult: invalid agg index"));
                }
                auto & state = agg_states_[agg_idx];
                if (state.func_type == 0)
                { // COUNT
                    push(popo::type::Value(popo::type::ValueType::Int64, state.count));
                }
                else if (state.func_type == 2)
                { // AVG
                    if (state.count > 0)
                    {
                        push(popo::type::Value(popo::type::ValueType::Float64, to_double(state.result) / static_cast<double>(state.count)));
                    }
                    else
                    {
                        push(popo::type::Value(popo::type::ValueType::Int64, static_cast<int64_t>(0)));
                    }
                }
                else
                {
                    push(state.result);
                }
                pc_++;
                break;
            }

            case OpCode::AggReset: {
                for (auto & state : agg_states_)
                {
                    state.count = 0;
                    state.initialized = false;
                    state.result = popo::type::Value();
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
                if (it != sorters_.end())
                {
                    if (inst.operand2 >= 0 && inst.operand2 < static_cast<int32_t>(it->second.sort_keys.size()))
                    {
                        it->second.sort_keys[inst.operand2].column_index = inst.operand2;
                        it->second.sort_keys[inst.operand2].ascending = (inst.operand3 != 0);
                    }
                }
                pc_++;
                break;
            }

            case OpCode::SorterInsert: {
                auto it = sorters_.find(inst.operand1);
                if (it == sorters_.end())
                {
                    return error(popo::base::Error(static_cast<int32_t>(popo::base::InternalErr), "SorterInsert: invalid sorter"));
                }
                int32_t num_vals = inst.operand2;
                auto row = std::make_shared<popo::type::Row>();
                row->reserve_values(num_vals);
                std::vector<popo::type::Value> vals(num_vals);
                for (int i = num_vals - 1; i >= 0; i--)
                {
                    vals[i] = pop();
                }
                for (auto & v : vals)
                {
                    row->append_value(std::move(v));
                }
                it->second.rows.push_back(row);
                pc_++;
                break;
            }

            case OpCode::SorterSort: {
                auto it = sorters_.find(inst.operand1);
                if (it != sorters_.end())
                {
                    auto & sorter = it->second;
                    std::sort(
                        sorter.rows.begin(),
                        sorter.rows.end(),
                        [&sorter](const auto & a, const auto & b)
                        {
                            for (const auto & key : sorter.sort_keys)
                            {
                                int32_t idx = key.column_index;
                                if (idx < 0 || idx >= static_cast<int32_t>(a->values_.size())
                                    || idx >= static_cast<int32_t>(b->values_.size()))
                                {
                                    continue;
                                }
                                const auto & av = a->values_[idx];
                                const auto & bv = b->values_[idx];
                                if (av == bv)
                                    continue;
                                if (key.ascending)
                                {
                                    return av < bv;
                                }
                                else
                                {
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
                if (it == sorters_.end() || it->second.rows.empty())
                {
                    pc_ = inst.operand2;
                }
                else
                {
                    it->second.current_row = 0;
                    pc_++;
                }
                break;
            }

            case OpCode::SorterNext: {
                auto it = sorters_.find(inst.operand1);
                if (it == sorters_.end())
                {
                    pc_++;
                    break;
                }
                auto & sorter = it->second;
                sorter.current_row++;
                if (sorter.current_row < static_cast<int32_t>(sorter.rows.size()))
                {
                    pc_ = inst.operand2;
                }
                else
                {
                    pc_++;
                }
                break;
            }

            case OpCode::SorterColumn: {
                auto it = sorters_.find(inst.operand1);
                if (it == sorters_.end())
                {
                    return error(popo::base::Error(static_cast<int32_t>(popo::base::InternalErr), "SorterColumn: invalid sorter"));
                }
                auto & sorter = it->second;
                if (sorter.current_row < 0 || sorter.current_row >= static_cast<int32_t>(sorter.rows.size()))
                {
                    return error(popo::base::Error(static_cast<int32_t>(popo::base::InternalErr), "SorterColumn: sorter out of bounds"));
                }
                auto & row = *sorter.rows[sorter.current_row];
                if (inst.operand2 < 0 || inst.operand2 >= static_cast<int32_t>(row.values_.size()))
                {
                    return error(
                        popo::base::Error(static_cast<int32_t>(popo::base::InternalErr), "SorterColumn: column index out of bounds"));
                }
                push(row.values_[inst.operand2]);
                pc_++;
                break;
            }

            case OpCode::CreateTable: {
                std::string table_name = std::get<std::string>(program.constants[inst.operand1]);
                auto col_defs = std::get<std::vector<popo::parser::ColumnDef>>(program.constants[inst.operand2]);
                std::vector<popo::catalog::Column> columns;
                for (const auto & def : col_defs)
                {
                    popo::catalog::Column col{};
                    col.column_name_ = def.name;
                    col.data_type_ = def.data_type;
                    col.type_len_ = def.type_len;
                    col.is_primary_key_ = def.is_primary_key;
                    col.is_nullable_ = def.is_primary_key ? false : def.is_nullable;
                    col.is_unique_ = def.is_unique;
                    if (def.default_value.has_value())
                    {
                        col.default_value_ = def.default_value.value();
                    }
                    columns.push_back(col);
                }
                auto create_result = schema_mgr_.create_table(table_name, columns);
                if (!create_result)
                {
                    return error(popo::base::Error(static_cast<int32_t>(popo::base::InternalErr), "Failed to create table: " + table_name));
                }
                // Create storage for the table
                auto qr = schema_mgr_.query_table(table_name);
                if (qr)
                {
                    engine_.create_table(table_name, *(*qr));
                }
                result_.message = "Table created";
                pc_++;
                break;
            }

            case OpCode::DropTable: {
                std::string table_name = std::get<std::string>(program.constants[inst.operand1]);
                bool if_exists = (inst.operand2 != 0);
                auto qr = schema_mgr_.query_table(table_name);
                if (!qr)
                {
                    if (if_exists)
                    {
                        result_.message = "Table does not exist";
                        pc_++;
                        break;
                    }
                    return error(popo::base::Error(static_cast<int32_t>(popo::base::TableNotFound), "Table not found: " + table_name));
                }
                auto del_result = schema_mgr_.delete_table(table_name);
                if (!del_result)
                {
                    return error(del_result.error());
                }
                engine_.drop_table(table_name);
                result_.message = "Table dropped";
                pc_++;
                break;
            }

            case OpCode::ShowTables: {
                for (const auto & name : schema_mgr_.all_table_names())
                {
                    auto row = std::make_shared<popo::type::Row>();
                    row->append_value(popo::type::Value(popo::type::ValueType::String, name));
                    result_.rows.push_back(row);
                }
                pc_++;
                break;
            }

            case OpCode::Describe: {
                std::string table_name = std::get<std::string>(program.constants[inst.operand1]);
                auto qr = schema_mgr_.query_table(table_name);
                if (qr && *qr)
                {
                    for (const auto & col : (*qr)->columns_)
                    {
                        auto row = std::make_shared<popo::type::Row>();
                        row->append_value(popo::type::Value(popo::type::ValueType::String, col.column_name_));
                        std::string type_name = "varchar(255)";
                        switch (col.data_type_)
                        {
                            case popo::type::ValueType::Int32:
                                type_name = "int";
                                break;
                            case popo::type::ValueType::Int64:
                                type_name = "bigint";
                                break;
                            case popo::type::ValueType::Float64:
                                type_name = "double";
                                break;
                            case popo::type::ValueType::Bool:
                                type_name = "tinyint";
                                break;
                            case popo::type::ValueType::String:
                                type_name = "varchar(1024)";
                                break;
                            default:
                                break;
                        }
                        row->append_value(popo::type::Value(popo::type::ValueType::String, type_name));
                        row->append_value(popo::type::Value(popo::type::ValueType::String, col.is_nullable_ ? "YES" : "NO"));
                        row->append_value(popo::type::Value(popo::type::ValueType::String, col.is_primary_key_ ? "PRI" : ""));
                        row->append_value(popo::type::Value(popo::type::ValueType::Null));
                        row->append_value(popo::type::Value(popo::type::ValueType::String, ""));
                        result_.rows.push_back(row);
                    }
                }
                pc_++;
                break;
            }

            case OpCode::ShowDatabases: {
                auto row = std::make_shared<popo::type::Row>();
                row->append_value(popo::type::Value(popo::type::ValueType::String, "popo"));
                result_.rows.push_back(row);
                pc_++;
                break;
            }

            case OpCode::ShowVariables: {
                // Empty result set with column headers only
                pc_++;
                break;
            }

            case OpCode::SetVar: {
                pc_++;
                break;
            }

            case OpCode::SelectDatabase: {
                auto row = std::make_shared<popo::type::Row>();
                row->append_value(popo::type::Value(popo::type::ValueType::String, "popo"));
                result_.rows.push_back(row);
                pc_++;
                break;
            }

            case OpCode::SelectSystemVar: {
                std::string var_name = std::get<std::string>(program.constants[inst.operand1]);
                auto row = std::make_shared<popo::type::Row>();
                if (var_name == "version_comment")
                {
                    row->append_value(popo::type::Value(popo::type::ValueType::String, "popo database"));
                }
                else if (var_name == "version")
                {
                    row->append_value(popo::type::Value(popo::type::ValueType::String, "0.1.0"));
                }
                else if (var_name == "max_allowed_packet")
                {
                    row->append_value(popo::type::Value(popo::type::ValueType::Int64, static_cast<int64_t>(16777216)));
                }
                else if (var_name == "autocommit")
                {
                    row->append_value(popo::type::Value(popo::type::ValueType::Int64, static_cast<int64_t>(1)));
                }
                else if (var_name == "sql_mode")
                {
                    row->append_value(
                        popo::type::Value(
                            popo::type::ValueType::String,
                            "ONLY_FULL_GROUP_BY,STRICT_TRANS_TABLES,NO_ZERO_IN_DATE,NO_ZERO_DATE,ERROR_FOR_DIVISION_BY_ZERO,NO_ENGINE_"
                            "SUBSTITUTION"));
                }
                else if (
                    var_name == "character_set_client" || var_name == "character_set_connection" || var_name == "character_set_results"
                    || var_name == "character_set_server")
                {
                    row->append_value(popo::type::Value(popo::type::ValueType::String, "utf8mb4"));
                }
                else if (var_name == "collation_connection" || var_name == "collation_server")
                {
                    row->append_value(popo::type::Value(popo::type::ValueType::String, "utf8mb4_general_ci"));
                }
                else if (var_name == "tx_isolation" || var_name == "transaction_isolation")
                {
                    row->append_value(popo::type::Value(popo::type::ValueType::String, "READ-COMMITTED"));
                }
                else
                {
                    row->append_value(popo::type::Value(popo::type::ValueType::String, ""));
                }
                result_.rows.push_back(row);
                pc_++;
                break;
            }

            case OpCode::Dup: {
                if (!stack_.empty())
                {
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
    if (own_txn_ && txn_)
    {
        engine_.commit_txn(txn_);
    }
    txn_ = nullptr;
    if (!result_.rows.empty())
    {
        result_.message = std::to_string(result_.rows.size()) + " rows in set";
    }
    else if (result_.rows_affected > 0)
    {
        result_.message = std::to_string(result_.rows_affected) + " rows affected";
    }
    else if (result_.message.empty())
    {
        result_.message = "OK";
    }
    return result_;
}

} // namespace popo::bytecode
