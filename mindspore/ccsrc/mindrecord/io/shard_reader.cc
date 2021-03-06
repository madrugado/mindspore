/**
 * Copyright 2019 Huawei Technologies Co., Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "mindrecord/include/shard_reader.h"
#include "common/utils.h"

using mindspore::LogStream;
using mindspore::ExceptionType::NoExceptionType;
using mindspore::MsLogLevel::DEBUG;
using mindspore::MsLogLevel::ERROR;
using mindspore::MsLogLevel::INFO;

namespace mindspore {
namespace mindrecord {
template <class Type>
// convert the string to exactly number type (int32_t/int64_t/float/double)
Type StringToNum(const std::string &str) {
  std::istringstream iss(str);
  Type num;
  iss >> num;
  return num;
}

ShardReader::ShardReader() {
  task_id_ = 0;
  deliver_id_ = 0;
  shard_count_ = 0;
  n_consumer_ = 0;
  page_size_ = 0;
  header_size_ = 0;
  num_rows_ = 0;
  row_id_ = 0;
  num_blocks_ = 0;
  block_reader_ = false;
}

MSRStatus ShardReader::Init(const std::string &file_path) {
  if (!IsLegalFile(file_path)) {
    return FAILED;
  }
  ShardHeader sh = ShardHeader();
  if (sh.Build(file_path) == FAILED) {
    return FAILED;
  }
  shard_header_ = std::make_shared<ShardHeader>(sh);
  header_size_ = shard_header_->get_header_size();
  page_size_ = shard_header_->get_page_size();
  file_paths_ = shard_header_->get_shard_addresses();

  for (const auto &file : file_paths_) {
    sqlite3 *db = nullptr;
    // sqlite3_open create a database if not found, use sqlite3_open_v2 instead of it
    int rc = sqlite3_open_v2(common::SafeCStr(file + ".db"), &db, SQLITE_OPEN_READONLY, nullptr);
    if (rc != SQLITE_OK) {
      MS_LOG(ERROR) << "Can't open database, error: " << sqlite3_errmsg(db);
      return FAILED;
    }
    MS_LOG(DEBUG) << "Opened database successfully";

    string sql = "select NAME from SHARD_NAME;";
    std::vector<std::vector<std::string>> name;
    char *errmsg = nullptr;
    rc = sqlite3_exec(db, common::SafeCStr(sql), SelectCallback, &name, &errmsg);
    if (rc != SQLITE_OK) {
      MS_LOG(ERROR) << "Error in select statement, sql: " << sql << ", error: " << errmsg;
      sqlite3_free(errmsg);
      sqlite3_close(db);
      return FAILED;
    } else {
      MS_LOG(DEBUG) << "Get " << static_cast<int>(name.size()) << " records from index.";
      string shardName = GetFileName(file).second;
      if (name.empty() || name[0][0] != shardName) {
        MS_LOG(ERROR) << "DB file can not match file " << file;
        sqlite3_free(errmsg);
        sqlite3_close(db);
        return FAILED;
      }
    }
    database_paths_.push_back(db);
  }

  num_rows_ = 0;
  auto row_group_summary = ReadRowGroupSummary();
  for (const auto &rg : row_group_summary) {
    num_rows_ += std::get<3>(rg);
  }

  MS_LOG(INFO) << "Get meta from mindrecord file & index file successfully.";

  return SUCCESS;
}

MSRStatus ShardReader::CheckColumnList(const std::vector<std::string> &selected_columns) {
  vector<int> inSchema(selected_columns.size(), 0);
  for (auto &p : get_shard_header()->get_schemas()) {
    auto schema = p->GetSchema()["schema"];
    for (unsigned int i = 0; i < selected_columns.size(); ++i) {
      if (schema.find(selected_columns[i]) != schema.end()) {
        inSchema[i] = 1;
      }
    }
  }
  if (std::any_of(std::begin(inSchema), std::end(inSchema), [](int x) { return x == 0; })) {
    return FAILED;
  }

  return SUCCESS;
}

MSRStatus ShardReader::Open() {
  file_streams_.clear();

  for (const auto &file : file_paths_) {
    std::shared_ptr<std::fstream> fs = std::make_shared<std::fstream>();
    fs->open(common::SafeCStr(file), std::ios::in | std::ios::binary);
    if (!fs->good()) {
      MS_LOG(ERROR) << "File could not opened";
      return FAILED;
    }
    MS_LOG(INFO) << "Open shard file successfully.";
    file_streams_.push_back(fs);
  }

  return SUCCESS;
}

MSRStatus ShardReader::Open(int n_consumer) {
  file_streams_random_ =
    std::vector<std::vector<std::shared_ptr<std::fstream>>>(n_consumer, std::vector<std::shared_ptr<std::fstream>>());
  for (const auto &file : file_paths_) {
    for (int j = 0; j < n_consumer; ++j) {
      std::shared_ptr<std::fstream> fs = std::make_shared<std::fstream>();
      fs->open(common::SafeCStr(file), std::ios::in | std::ios::binary);
      if (!fs->good()) {
        MS_LOG(ERROR) << "File could not opened";
        return FAILED;
      }
      file_streams_random_[j].push_back(fs);
    }
    MS_LOG(INFO) << "Open shard file successfully.";
  }

  return SUCCESS;
}

void ShardReader::FileStreamsOperator() {
  for (int i = static_cast<int>(file_streams_.size()) - 1; i >= 0; --i) {
    if (file_streams_[i] != nullptr) {
      file_streams_[i]->close();
    }
  }
  for (int i = static_cast<int>(file_streams_random_.size()) - 1; i >= 0; --i) {
    for (int j = static_cast<int>(file_streams_random_[i].size()) - 1; j >= 0; --j) {
      if (file_streams_random_[i][j] != nullptr) {
        file_streams_random_[i][j]->close();
      }
    }
  }
  for (int i = static_cast<int>(database_paths_.size()) - 1; i >= 0; --i) {
    if (database_paths_[i] != nullptr) {
      (void)sqlite3_close(database_paths_[i]);
    }
  }
}

ShardReader::~ShardReader() { Close(); }

void ShardReader::Close() {
  (void)Finish();  // interrupt reading and stop threads
  FileStreamsOperator();
}

std::shared_ptr<ShardHeader> ShardReader::get_shard_header() const { return shard_header_; }

int ShardReader::get_shard_count() const { return shard_header_->get_shard_count(); }

int ShardReader::get_num_rows() const { return num_rows_; }

std::vector<std::tuple<int, int, int, uint64_t>> ShardReader::ReadRowGroupSummary() {
  std::vector<std::tuple<int, int, int, uint64_t>> row_group_summary;
  int shard_count = shard_header_->get_shard_count();
  if (shard_count <= 0) {
    return row_group_summary;
  }
  if (shard_count <= kMaxShardCount) {
    for (int shard_id = 0; shard_id < shard_count; ++shard_id) {
      // return -1 when page's size equals to 0.
      auto last_page_id = shard_header_->GetLastPageId(shard_id);
      if (static_cast<int>(last_page_id) == -1) {
        continue;
      }
      for (uint64_t page_id = 0; page_id <= last_page_id; ++page_id) {
        const auto &page_t = shard_header_->GetPage(shard_id, page_id);
        const auto &page = page_t.first;
        if (page->get_page_type() != kPageTypeBlob) continue;
        uint64_t start_row_id = page->get_start_row_id();
        if (start_row_id > page->get_end_row_id()) {
          return std::vector<std::tuple<int, int, int, uint64_t>>();
        }
        uint64_t number_of_rows = page->get_end_row_id() - start_row_id;
        row_group_summary.emplace_back(shard_id, page->get_page_type_id(), start_row_id, number_of_rows);
      }
    }
  }
  return row_group_summary;
}

MSRStatus ShardReader::ConvertLabelToJson(const std::vector<std::vector<std::string>> &labels,
                                          std::shared_ptr<std::fstream> fs,
                                          std::vector<std::vector<std::vector<uint64_t>>> &offsets, int shard_id,
                                          const std::vector<std::string> &columns,
                                          std::vector<std::vector<json>> &column_values) {
  for (int i = 0; i < static_cast<int>(labels.size()); ++i) {
    uint64_t group_id = std::stoull(labels[i][0]);
    uint64_t offset_start = std::stoull(labels[i][1]) + kInt64Len;
    uint64_t offset_end = std::stoull(labels[i][2]);
    offsets[shard_id].emplace_back(
      std::vector<uint64_t>{static_cast<uint64_t>(shard_id), group_id, offset_start, offset_end});
    if (!all_in_index_) {
      int raw_page_id = std::stoi(labels[i][3]);
      uint64_t label_start = std::stoull(labels[i][4]) + kInt64Len;
      uint64_t label_end = std::stoull(labels[i][5]);
      auto len = label_end - label_start;
      auto label_raw = std::vector<uint8_t>(len);
      auto &io_seekg = fs->seekg(page_size_ * raw_page_id + header_size_ + label_start, std::ios::beg);
      if (!io_seekg.good() || io_seekg.fail() || io_seekg.bad()) {
        MS_LOG(ERROR) << "File seekg failed";
        fs->close();
        return FAILED;
      }

      auto &io_read = fs->read(reinterpret_cast<char *>(&label_raw[0]), len);
      if (!io_read.good() || io_read.fail() || io_read.bad()) {
        MS_LOG(ERROR) << "File read failed";
        fs->close();
        return FAILED;
      }

      json label_json = json::from_msgpack(label_raw);
      json tmp;
      if (!columns.empty()) {
        for (auto &col : columns) {
          if (label_json.find(col) != label_json.end()) {
            tmp[col] = label_json[col];
          }
        }
      } else {
        tmp = label_json;
      }
      column_values[shard_id].emplace_back(tmp);
    } else {
      json construct_json;
      for (unsigned int j = 0; j < columns.size(); ++j) {
        // construct json "f1": value
        auto schema = shard_header_->get_schemas()[0]->GetSchema()["schema"];

        // convert the string to base type by schema
        if (schema[columns[j]]["type"] == "int32") {
          construct_json[columns[j]] = StringToNum<int32_t>(labels[i][j + 3]);
        } else if (schema[columns[j]]["type"] == "int64") {
          construct_json[columns[j]] = StringToNum<int64_t>(labels[i][j + 3]);
        } else if (schema[columns[j]]["type"] == "float32") {
          construct_json[columns[j]] = StringToNum<float>(labels[i][j + 3]);
        } else if (schema[columns[j]]["type"] == "float64") {
          construct_json[columns[j]] = StringToNum<double>(labels[i][j + 3]);
        } else {
          construct_json[columns[j]] = std::string(labels[i][j + 3]);
        }
      }
      column_values[shard_id].emplace_back(construct_json);
    }
  }

  return SUCCESS;
}

MSRStatus ShardReader::ReadAllRowsInShard(int shard_id, const std::string &sql, const std::vector<std::string> &columns,
                                          std::vector<std::vector<std::vector<uint64_t>>> &offsets,
                                          std::vector<std::vector<json>> &column_values) {
  auto db = database_paths_[shard_id];
  std::vector<std::vector<std::string>> labels;
  char *errmsg = nullptr;
  int rc = sqlite3_exec(db, common::SafeCStr(sql), SelectCallback, &labels, &errmsg);
  if (rc != SQLITE_OK) {
    MS_LOG(ERROR) << "Error in select statement, sql: " << sql << ", error: " << errmsg;
    sqlite3_free(errmsg);
    sqlite3_close(db);
    return FAILED;
  }
  MS_LOG(INFO) << "Get " << static_cast<int>(labels.size()) << " records from shard " << shard_id << " index.";

  std::string file_name = file_paths_[shard_id];
  std::shared_ptr<std::fstream> fs = std::make_shared<std::fstream>();
  if (!all_in_index_) {
    fs->open(common::SafeCStr(file_name), std::ios::in | std::ios::binary);
    if (!fs->good()) {
      MS_LOG(ERROR) << "File could not opened";
      return FAILED;
    }
  }
  sqlite3_free(errmsg);
  return ConvertLabelToJson(labels, fs, offsets, shard_id, columns, column_values);
}

ROW_GROUPS ShardReader::ReadAllRowGroup(std::vector<std::string> &columns) {
  std::string fields = "ROW_GROUP_ID, PAGE_OFFSET_BLOB, PAGE_OFFSET_BLOB_END";
  std::vector<std::vector<std::vector<uint64_t>>> offsets(shard_count_, std::vector<std::vector<uint64_t>>{});
  std::vector<std::vector<json>> column_values(shard_count_, std::vector<json>{});
  if (all_in_index_) {
    for (unsigned int i = 0; i < columns.size(); ++i) {
      fields += ',';
      auto ret = ShardIndexGenerator::GenerateFieldName(std::make_pair(column_schema_id_[columns[i]], columns[i]));
      if (ret.first != SUCCESS) {
        return std::make_tuple(FAILED, std::move(offsets), std::move(column_values));
      }
      fields += ret.second;
    }
  } else {  // fetch raw data from Raw page while some field is not index.
    fields += ", PAGE_ID_RAW, PAGE_OFFSET_RAW, PAGE_OFFSET_RAW_END ";
  }

  std::string sql = "SELECT " + fields + " FROM INDEXES ORDER BY ROW_ID ;";

  std::vector<std::thread> thread_read_db = std::vector<std::thread>(shard_count_);
  for (int x = 0; x < shard_count_; x++) {
    thread_read_db[x] =
      std::thread(&ShardReader::ReadAllRowsInShard, this, x, sql, columns, std::ref(offsets), std::ref(column_values));
  }

  for (int x = 0; x < shard_count_; x++) {
    thread_read_db[x].join();
  }
  return std::make_tuple(SUCCESS, std::move(offsets), std::move(column_values));
}

ROW_GROUP_BRIEF ShardReader::ReadRowGroupBrief(int group_id, int shard_id, const std::vector<std::string> &columns) {
  std::lock_guard<std::mutex> lck(shard_locker_);
  const auto &ret = shard_header_->GetPageByGroupId(group_id, shard_id);
  if (SUCCESS != ret.first) {
    return std::make_tuple(FAILED, "", 0, 0, std::vector<std::vector<uint64_t>>(), std::vector<json>());
  }
  const std::shared_ptr<Page> &page = ret.second;
  std::string file_name = file_paths_[shard_id];
  uint64_t page_length = page->get_page_size();
  uint64_t page_offset = page_size_ * page->get_page_id() + header_size_;
  std::vector<std::vector<uint64_t>> image_offset = GetImageOffset(page->get_page_id(), shard_id);

  auto status_labels = GetLabels(page->get_page_id(), shard_id, columns);
  if (status_labels.first != SUCCESS) {
    return std::make_tuple(FAILED, "", 0, 0, std::vector<std::vector<uint64_t>>(), std::vector<json>());
  }
  return std::make_tuple(SUCCESS, file_name, page_length, page_offset, std::move(image_offset),
                         std::move(status_labels.second));
}

ROW_GROUP_BRIEF ShardReader::ReadRowGroupCriteria(int group_id, int shard_id,
                                                  const std::pair<std::string, std::string> &criteria,
                                                  const std::vector<std::string> &columns) {
  std::lock_guard<std::mutex> lck(shard_locker_);
  const auto &ret = shard_header_->GetPageByGroupId(group_id, shard_id);
  if (SUCCESS != ret.first) {
    return std::make_tuple(FAILED, "", 0, 0, std::vector<std::vector<uint64_t>>(), std::vector<json>());
  }
  vector<string> criteria_list{criteria.first};
  if (CheckColumnList(criteria_list) == FAILED) {
    return std::make_tuple(FAILED, "", 0, 0, std::vector<std::vector<uint64_t>>(), std::vector<json>());
  }
  const std::shared_ptr<Page> &page = ret.second;
  std::string file_name = file_paths_[shard_id];
  uint64_t page_length = page->get_page_size();
  uint64_t page_offset = page_size_ * page->get_page_id() + header_size_;
  std::vector<std::vector<uint64_t>> image_offset = GetImageOffset(page->get_page_id(), shard_id, criteria);

  auto status_labels = GetLabels(page->get_page_id(), shard_id, columns, criteria);
  if (status_labels.first != SUCCESS) {
    return std::make_tuple(FAILED, "", 0, 0, std::vector<std::vector<uint64_t>>(), std::vector<json>());
  }

  return std::make_tuple(SUCCESS, file_name, page_length, page_offset, std::move(image_offset),
                         std::move(status_labels.second));
}

int ShardReader::SelectCallback(void *p_data, int num_fields, char **p_fields, char **p_col_names) {
  auto *records = static_cast<std::vector<std::vector<std::string>> *>(p_data);
  if (num_fields > 0 && num_fields <= kMaxFieldCount) {
    for (int i = 0; i < num_fields; ++i)
      if (p_fields[i] == nullptr) p_fields[i] = const_cast<char *>("");
  }
  records->emplace_back(p_fields, p_fields + num_fields);
  return 0;
}

std::vector<std::vector<uint64_t>> ShardReader::GetImageOffset(int page_id, int shard_id,
                                                               const std::pair<std::string, std::string> &criteria) {
  auto db = database_paths_[shard_id];

  std::string sql =
    "SELECT PAGE_OFFSET_BLOB, PAGE_OFFSET_BLOB_END FROM INDEXES WHERE PAGE_ID_BLOB = " + std::to_string(page_id);

  // whether use index search
  if (!criteria.first.empty()) {
    auto schema = shard_header_->get_schemas()[0]->GetSchema();

    // not number field should add '' in sql
    if (kNumberFieldTypeSet.find(schema["schema"][criteria.first]["type"]) != kNumberFieldTypeSet.end()) {
      sql +=
        " AND " + criteria.first + "_" + std::to_string(column_schema_id_[criteria.first]) + " = " + criteria.second;
    } else {
      sql += " AND " + criteria.first + "_" + std::to_string(column_schema_id_[criteria.first]) + " = '" +
             criteria.second + "'";
    }
  }
  sql += ";";
  std::vector<std::vector<std::string>> image_offsets;
  char *errmsg = nullptr;
  int rc = sqlite3_exec(db, common::SafeCStr(sql), SelectCallback, &image_offsets, &errmsg);
  if (rc != SQLITE_OK) {
    MS_LOG(ERROR) << "Error in select statement, sql: " << sql << ", error: " << errmsg;
    sqlite3_free(errmsg);
    sqlite3_close(db);
    return std::vector<std::vector<uint64_t>>();
  } else {
    MS_LOG(DEBUG) << "Get " << static_cast<int>(image_offsets.size()) << "records from index.";
  }
  std::vector<std::vector<uint64_t>> res;
  for (int i = static_cast<int>(image_offsets.size()) - 1; i >= 0; i--) res.emplace_back(std::vector<uint64_t>{0, 0});
  for (int i = 0; i < static_cast<int>(image_offsets.size()); i++) {
    const auto &image_offset = image_offsets[i];
    res[i][0] = std::stoull(image_offset[0]) + kInt64Len;
    res[i][1] = std::stoull(image_offset[1]);
  }
  sqlite3_free(errmsg);
  return res;
}

void ShardReader::CheckNlp() {
  nlp_ = false;
  return;
}

bool ShardReader::get_nlp_flag() { return nlp_; }

std::pair<ShardType, std::vector<std::string>> ShardReader::get_blob_fields() {
  std::vector<std::string> blob_fields;
  for (auto &p : get_shard_header()->get_schemas()) {
    // assume one schema
    const auto &fields = p->get_blob_fields();
    blob_fields.assign(fields.begin(), fields.end());
    break;
  }
  return std::make_pair(nlp_ ? kNLP : kCV, blob_fields);
}

void ShardReader::CheckIfColumnInIndex(const std::vector<std::string> &columns) {
  // assume different schemas do not contain same key.
  if (columns.empty()) {
    all_in_index_ = false;
    return;
  }
  for (auto &field : get_shard_header()->get_fields()) {
    column_schema_id_[field.second] = field.first;
  }
  for (auto &col : columns) {
    if (column_schema_id_.find(col) == column_schema_id_.end()) {
      all_in_index_ = false;
      return;
    }
  }
}

MSRStatus ShardReader::QueryWithCriteria(sqlite3 *db, string &sql, string criteria,
                                         std::vector<std::vector<std::string>> &labels) {
  sqlite3_stmt *stmt = nullptr;
  if (sqlite3_prepare_v2(db, common::SafeCStr(sql), -1, &stmt, 0) != SQLITE_OK) {
    MS_LOG(ERROR) << "SQL error: could not prepare statement";
    return FAILED;
  }
  int index = sqlite3_bind_parameter_index(stmt, ":criteria");
  if (sqlite3_bind_text(stmt, index, common::SafeCStr(criteria), -1, SQLITE_STATIC) != SQLITE_OK) {
    MS_LOG(ERROR) << "SQL error: could not bind parameter, index: " << index << ", field value: " << criteria;
    return FAILED;
  }
  int rc = sqlite3_step(stmt);
  while (rc != SQLITE_DONE) {
    vector<string> tmp;
    int ncols = sqlite3_column_count(stmt);
    for (int i = 0; i < ncols; i++) {
      tmp.emplace_back(reinterpret_cast<const char *>(sqlite3_column_text(stmt, i)));
    }
    labels.push_back(tmp);
    rc = sqlite3_step(stmt);
  }
  (void)sqlite3_finalize(stmt);
  return SUCCESS;
}

std::pair<MSRStatus, std::vector<json>> ShardReader::GetLabelsFromBinaryFile(
  int shard_id, const std::vector<std::string> &columns, const std::vector<std::vector<std::string>> &label_offsets) {
  std::string file_name = file_paths_[shard_id];
  std::vector<json> res;
  std::shared_ptr<std::fstream> fs = std::make_shared<std::fstream>();
  fs->open(common::SafeCStr(file_name), std::ios::in | std::ios::binary);
  if (!fs->good()) {
    MS_LOG(ERROR) << "File could not opened";
    return {FAILED, {}};
  }

  // init the return
  for (unsigned int i = 0; i < label_offsets.size(); ++i) {
    res.emplace_back(json{});
  }

  for (unsigned int i = 0; i < label_offsets.size(); ++i) {
    const auto &labelOffset = label_offsets[i];
    uint64_t label_start = std::stoull(labelOffset[1]) + kInt64Len;
    uint64_t label_end = std::stoull(labelOffset[2]);
    int raw_page_id = std::stoi(labelOffset[0]);
    auto len = label_end - label_start;
    auto label_raw = std::vector<uint8_t>(len);
    auto &io_seekg = fs->seekg(page_size_ * raw_page_id + header_size_ + label_start, std::ios::beg);
    if (!io_seekg.good() || io_seekg.fail() || io_seekg.bad()) {
      MS_LOG(ERROR) << "File seekg failed";
      fs->close();
      return {FAILED, {}};
    }

    auto &io_read = fs->read(reinterpret_cast<char *>(&label_raw[0]), len);
    if (!io_read.good() || io_read.fail() || io_read.bad()) {
      MS_LOG(ERROR) << "File read failed";
      fs->close();
      return {FAILED, {}};
    }

    json label_json = json::from_msgpack(label_raw);
    json tmp = label_json;
    for (auto &col : columns) {
      if (label_json.find(col) != label_json.end()) {
        tmp[col] = label_json[col];
      }
    }
    res[i] = tmp;
  }
  return {SUCCESS, res};
}

std::pair<MSRStatus, std::vector<json>> ShardReader::GetLabelsFromPage(
  int page_id, int shard_id, const std::vector<std::string> &columns,
  const std::pair<std::string, std::string> &criteria) {
  // get page info from sqlite
  auto db = database_paths_[shard_id];
  std::string sql = "SELECT PAGE_ID_RAW, PAGE_OFFSET_RAW,PAGE_OFFSET_RAW_END FROM INDEXES WHERE PAGE_ID_BLOB = " +
                    std::to_string(page_id);
  std::vector<std::vector<std::string>> label_offsets;
  if (!criteria.first.empty()) {
    sql += " AND " + criteria.first + "_" + std::to_string(column_schema_id_[criteria.first]) + " = :criteria";
    if (QueryWithCriteria(db, sql, criteria.second, label_offsets) == FAILED) {
      return {FAILED, {}};
    }
  } else {
    sql += ";";
    char *errmsg = nullptr;
    int rc = sqlite3_exec(db, common::SafeCStr(sql), SelectCallback, &label_offsets, &errmsg);
    if (rc != SQLITE_OK) {
      MS_LOG(ERROR) << "Error in select statement, sql: " << sql << ", error: " << errmsg;
      sqlite3_free(errmsg);
      sqlite3_close(db);
      return {FAILED, {}};
    }
    MS_LOG(DEBUG) << "Get " << label_offsets.size() << "records from index.";
    sqlite3_free(errmsg);
  }
  // get labels from binary file
  return GetLabelsFromBinaryFile(shard_id, columns, label_offsets);
}

std::pair<MSRStatus, std::vector<json>> ShardReader::GetLabels(int page_id, int shard_id,
                                                               const std::vector<std::string> &columns,
                                                               const std::pair<std::string, std::string> &criteria) {
  if (all_in_index_) {
    auto db = database_paths_[shard_id];
    std::string fields;
    for (unsigned int i = 0; i < columns.size(); ++i) {
      if (i > 0) fields += ',';
      uint64_t schema_id = column_schema_id_[columns[i]];
      fields += columns[i] + "_" + std::to_string(schema_id);
    }
    if (fields.empty()) fields = "*";
    std::vector<std::vector<std::string>> labels;
    std::string sql = "SELECT " + fields + " FROM INDEXES WHERE PAGE_ID_BLOB = " + std::to_string(page_id);
    if (!criteria.first.empty()) {
      sql += " AND " + criteria.first + "_" + std::to_string(column_schema_id_[criteria.first]) + " = " + ":criteria";
      if (QueryWithCriteria(db, sql, criteria.second, labels) == FAILED) {
        return {FAILED, {}};
      }
    } else {
      sql += ";";
      char *errmsg = nullptr;
      int rc = sqlite3_exec(db, common::SafeCStr(sql), SelectCallback, &labels, &errmsg);
      if (rc != SQLITE_OK) {
        MS_LOG(ERROR) << "Error in select statement, sql: " << sql << ", error: " << errmsg;
        sqlite3_free(errmsg);
        sqlite3_close(db);
        return {FAILED, {}};
      } else {
        MS_LOG(DEBUG) << "Get " << static_cast<int>(labels.size()) << "records from index.";
      }
      sqlite3_free(errmsg);
    }
    std::vector<json> ret;
    for (unsigned int i = 0; i < labels.size(); ++i) ret.emplace_back(json{});
    for (unsigned int i = 0; i < labels.size(); ++i) {
      json construct_json;
      for (unsigned int j = 0; j < columns.size(); ++j) {
        // construct json "f1": value
        auto schema = shard_header_->get_schemas()[0]->GetSchema()["schema"];

        // convert the string to base type by schema
        if (schema[columns[j]]["type"] == "int32") {
          construct_json[columns[j]] = StringToNum<int32_t>(labels[i][j]);
        } else if (schema[columns[j]]["type"] == "int64") {
          construct_json[columns[j]] = StringToNum<int64_t>(labels[i][j]);
        } else if (schema[columns[j]]["type"] == "float32") {
          construct_json[columns[j]] = StringToNum<float>(labels[i][j]);
        } else if (schema[columns[j]]["type"] == "float64") {
          construct_json[columns[j]] = StringToNum<double>(labels[i][j]);
        } else {
          construct_json[columns[j]] = std::string(labels[i][j]);
        }
      }
      ret[i] = construct_json;
    }
    return {SUCCESS, ret};
  }
  return GetLabelsFromPage(page_id, shard_id, columns, criteria);
}

bool ResortRowGroups(std::tuple<int, int, int, int> a, std::tuple<int, int, int, int> b) {
  return std::get<1>(a) < std::get<1>(b) || (std::get<1>(a) == std::get<1>(b) && std::get<0>(a) < std::get<0>(b));
}

MSRStatus ShardReader::Finish() {
  {
    std::lock_guard<std::mutex> lck(mtx_delivery_);
    interrupt_ = true;
  }
  cv_delivery_.notify_all();

  // Wait for all threads to finish
  for (auto &i_thread : thread_set_) {
    if (i_thread.joinable()) {
      i_thread.join();
    }
  }
  return SUCCESS;
}

MSRStatus ShardReader::CountTotalRows(const std::string &file_path, int64_t *count) {
  if (Init(file_path) == FAILED) {
    return FAILED;
  }
  *count = num_rows_;
  return SUCCESS;
}

MSRStatus ShardReader::Open(const std::string &file_path, int n_consumer,
                            const std::vector<std::string> &selected_columns,
                            const std::vector<std::shared_ptr<ShardOperator>> &operators, const bool &block_reader) {
  // Open file and set header by ShardReader
  if (Init(file_path) == FAILED) {
    return FAILED;
  }
  auto thread_limit = GetMaxThreadNum();
  if (n_consumer > thread_limit) {
    n_consumer = thread_limit;
  }
  if (n_consumer < kMinConsumerCount) {
    n_consumer = kMinConsumerCount;
  }
  CheckNlp();
  if (nlp_) {
    selected_columns_ = selected_columns;
  } else {
    vector<std::string> blob_fields = get_blob_fields().second;
    for (unsigned int i = 0; i < selected_columns.size(); ++i) {
      if (!std::any_of(blob_fields.begin(), blob_fields.end(),
                       [&selected_columns, i](std::string item) { return selected_columns[i] == item; })) {
        selected_columns_.push_back(selected_columns[i]);
      }
    }
  }

  if (CheckColumnList(selected_columns_) == FAILED) {
    MS_LOG(ERROR) << "Illegal column list";
    return ILLEGAL_COLUMN_LIST;
  }

  // Initialize argument
  shard_count_ = static_cast<int>(file_paths_.size());
  n_consumer_ = n_consumer;

  operators_ = operators;

  if (block_reader) {
    block_reader_ = true;
    if (Open() == FAILED) {
      return FAILED;
    }
    delivery_block_ = std::vector<std::shared_ptr<std::pair<std::vector<std::vector<uint64_t>>, std::vector<json>>>>(
      kNumPageInBuffer, std::shared_ptr<std::pair<std::vector<std::vector<uint64_t>>, std::vector<json>>>{});
    buf_ = std::vector<std::vector<uint8_t>>(kNumPageInBuffer, std::vector<uint8_t>(page_size_));
  } else {
    block_reader_ = false;
    if (Open(n_consumer) == FAILED) {
      return FAILED;
    }
  }
  return SUCCESS;
}

MSRStatus ShardReader::OpenPy(const std::string &file_path, const int &n_consumer,
                              const std::vector<std::string> &selected_columns,
                              const std::vector<std::shared_ptr<ShardOperator>> &operators) {
  // Open file and set header by ShardReader
  if (Init(file_path) == FAILED) {
    return FAILED;
  }
  // should remove blob field from selected_columns when call from python
  std::vector<std::string> columns(selected_columns);
  auto blob_fields = get_blob_fields().second;
  for (auto &blob_field : blob_fields) {
    auto it = std::find(selected_columns.begin(), selected_columns.end(), blob_field);
    if (it != selected_columns.end()) {
      columns.erase(columns.begin() + std::distance(selected_columns.begin(), it));
    }
  }
  if (CheckColumnList(columns) == FAILED) {
    MS_LOG(ERROR) << "Illegal column list";
    return FAILED;
  }
  if (Open(n_consumer) == FAILED) {
    return FAILED;
  }
  CheckNlp();
  // Initialize argument
  shard_count_ = static_cast<int>(file_paths_.size());
  n_consumer_ = n_consumer;

  // Initialize columns which will be read
  selected_columns_ = selected_columns;
  operators_ = operators;

  return SUCCESS;
}

MSRStatus ShardReader::Launch(bool isSimpleReader) {
  // Get all row groups' info
  auto row_group_summary = ReadRowGroupSummary();

  // Sort row group by (group_id, shard_id), prepare for parallel reading
  std::sort(row_group_summary.begin(), row_group_summary.end(), ResortRowGroups);
  if (CreateTasks(row_group_summary, operators_) != SUCCESS) {
    MS_LOG(ERROR) << "Failed to launch read threads.";
    interrupt_ = true;
    return FAILED;
  }
  MS_LOG(INFO) << "Launching read threads.";

  if (isSimpleReader) return SUCCESS;

  // Start provider consumer threads
  thread_set_ = std::vector<std::thread>(n_consumer_);
  if (n_consumer_ <= 0 || n_consumer_ > kMaxConsumerCount) {
    return FAILED;
  }

  for (int x = 0; x < n_consumer_; ++x) {
    if (block_reader_) {
      thread_set_[x] = std::thread(&ShardReader::ConsumerByBlock, this, x);
    } else {
      thread_set_[x] = std::thread(&ShardReader::ConsumerByRow, this, x);
    }
  }
  return SUCCESS;
}

vector<std::string> ShardReader::GetAllColumns() {
  vector<std::string> columns;
  if (nlp_) {
    for (auto &c : selected_columns_) {
      for (auto &p : get_shard_header()->get_schemas()) {
        auto schema = p->GetSchema()["schema"];  // make sure schema is not reference since error occurred in arm.
        for (auto it = schema.begin(); it != schema.end(); ++it) {
          if (it.key() == c) {
            columns.push_back(c);
          }
        }
      }
    }
  } else {
    columns = selected_columns_;
  }
  return std::move(columns);
}

MSRStatus ShardReader::CreateTasksByBlock(const std::vector<std::tuple<int, int, int, uint64_t>> &row_group_summary,
                                          const std::vector<std::shared_ptr<ShardOperator>> &operators) {
  vector<std::string> columns = GetAllColumns();
  CheckIfColumnInIndex(columns);
  for (const auto &rg : row_group_summary) {
    auto shard_id = std::get<0>(rg);
    auto group_id = std::get<1>(rg);
    auto n_Rows = std::get<3>(rg);
    tasks_.InsertTask(shard_id, group_id, std::vector<uint64_t>{n_Rows}, json{});
  }
  return SUCCESS;
}

int ShardReader::CreateTasksByCategory(const std::vector<std::tuple<int, int, int, uint64_t>> &row_group_summary,
                                       const std::vector<std::shared_ptr<ShardOperator>> &operators) {
  vector<std::string> columns = GetAllColumns();
  CheckIfColumnInIndex(columns);

  int category_operator = -1;
  for (uint32_t i = 0; i < operators.size(); ++i) {
    const auto &op = operators[i];
    if (std::dynamic_pointer_cast<ShardCategory>(op)) category_operator = static_cast<int>(i);
  }

  if (category_operator == -1) return category_operator;

  auto categories = std::dynamic_pointer_cast<ShardCategory>(operators[category_operator])->get_categories();

  // Generate task list, a task will create a batch
  std::vector<ShardTask> categoryTasks(categories.size());
  for (uint32_t categoryNo = 0; categoryNo < categories.size(); ++categoryNo) {
    for (const auto &rg : row_group_summary) {
      auto shard_id = std::get<0>(rg);
      auto group_id = std::get<1>(rg);

      auto details = ReadRowGroupCriteria(group_id, shard_id, categories[categoryNo], columns);
      if (SUCCESS != std::get<0>(details)) {
        return -2;
      }
      auto offsets = std::get<4>(details);

      auto number_of_rows = offsets.size();
      for (uint32_t iStart = 0; iStart < number_of_rows; iStart += 1) {
        categoryTasks[categoryNo].InsertTask(shard_id, group_id, std::get<4>(details)[iStart],
                                             std::get<5>(details)[iStart]);
      }
    }
    MS_LOG(INFO) << "Category #" << categoryNo << " has " << categoryTasks[categoryNo].Size() << " tasks";
  }
  tasks_ = ShardTask::Combine(categoryTasks);
  return category_operator;
}

MSRStatus ShardReader::CreateTasksByRow(const std::vector<std::tuple<int, int, int, uint64_t>> &row_group_summary,
                                        const std::vector<std::shared_ptr<ShardOperator>> &operators) {
  vector<std::string> columns = GetAllColumns();
  CheckIfColumnInIndex(columns);

  auto ret = ReadAllRowGroup(columns);
  if (std::get<0>(ret) != SUCCESS) {
    return FAILED;
  }
  auto offsets = std::get<1>(ret);
  auto local_columns = std::get<2>(ret);
  if (shard_count_ <= kMaxShardCount) {
    for (int shard_id = 0; shard_id < shard_count_; shard_id++) {
      for (uint32_t i = 0; i < offsets[shard_id].size(); i += 1) {
        tasks_.InsertTask(offsets[shard_id][i][0], offsets[shard_id][i][1],
                          std::vector<uint64_t>{offsets[shard_id][i][2], offsets[shard_id][i][3]},
                          local_columns[shard_id][i]);
      }
    }
  } else {
    return FAILED;
  }
  return SUCCESS;
}

MSRStatus ShardReader::CreateTasks(const std::vector<std::tuple<int, int, int, uint64_t>> &row_group_summary,
                                   const std::vector<std::shared_ptr<ShardOperator>> &operators) {
  if (block_reader_) {
    CreateTasksByBlock(row_group_summary, operators);
  } else {
    int category_operator = CreateTasksByCategory(row_group_summary, operators);
    if (category_operator == -1) {
      CreateTasksByRow(row_group_summary, operators);
    }
    if (category_operator == -2) {
      return FAILED;
    }
  }

  for (uint32_t operator_no = 0; operator_no < operators.size(); operator_no++) {
    const auto &op = operators[operator_no];
    if (std::dynamic_pointer_cast<ShardCategory>(op)) continue;
    if (block_reader_ && std::dynamic_pointer_cast<ShardShuffle>(op)) continue;
    if (SUCCESS != (*op)(tasks_)) {
      return FAILED;
    }
  }

  if (tasks_.permutation_.empty()) tasks_.MakePerm();
  num_rows_ = block_reader_ ? tasks_.SizeOfRows() : tasks_.Size();
  num_blocks_ = block_reader_ ? tasks_.Size() : 0;
  MS_LOG(INFO) << "Total rows is " << num_rows_;
  return SUCCESS;
}

TASK_RETURN_CONTENT ShardReader::ConsumerOneTask(int task_id, uint32_t consumer_id) {
  // All tasks are done
  if (task_id >= static_cast<int>(tasks_.Size())) {
    return std::make_pair(FAILED, std::vector<std::tuple<std::vector<uint8_t>, json>>());
  }

  // Pick up task from task list
  auto task = tasks_.get_task_by_id(tasks_.permutation_[task_id]);

  auto shard_id = std::get<0>(std::get<0>(task));
  auto group_id = std::get<1>(std::get<0>(task));
  auto addr = std::get<1>(task);
  const auto &ret = shard_header_->GetPageByGroupId(group_id, shard_id);
  if (SUCCESS != ret.first) {
    return std::make_pair(FAILED, std::vector<std::tuple<std::vector<uint8_t>, json>>());
  }
  const std::shared_ptr<Page> &page = ret.second;
  // Pack image list
  std::vector<uint8_t> images(addr[1] - addr[0]);
  auto file_offset = header_size_ + page_size_ * (page->get_page_id()) + addr[0];

  auto &io_seekg = file_streams_random_[consumer_id][shard_id]->seekg(file_offset, std::ios::beg);
  if (!io_seekg.good() || io_seekg.fail() || io_seekg.bad()) {
    MS_LOG(ERROR) << "File seekg failed";
    file_streams_random_[consumer_id][shard_id]->close();
    return std::make_pair(FAILED, std::vector<std::tuple<std::vector<uint8_t>, json>>());
  }

  auto &io_read =
    file_streams_random_[consumer_id][shard_id]->read(reinterpret_cast<char *>(&images[0]), addr[1] - addr[0]);
  if (!io_read.good() || io_read.fail() || io_read.bad()) {
    MS_LOG(ERROR) << "File read failed";
    file_streams_random_[consumer_id][shard_id]->close();
    return std::make_pair(FAILED, std::vector<std::tuple<std::vector<uint8_t>, json>>());
  }

  // Deliver batch data to output map
  std::vector<std::tuple<std::vector<uint8_t>, json>> batch;
  if (nlp_) {
    json blob_fields = json::from_msgpack(images);

    json merge;
    if (selected_columns_.size() > 0) {
      for (auto &col : selected_columns_) {
        if (blob_fields.find(col) != blob_fields.end()) {
          merge[col] = blob_fields[col];
        }
      }
    } else {
      merge = blob_fields;
    }
    auto label_json = std::get<2>(task);
    if (label_json != nullptr) {
      merge.update(label_json);
    }
    batch.emplace_back(std::vector<uint8_t>{}, std::move(merge));
  } else {
    batch.emplace_back(std::move(images), std::move(std::get<2>(task)));
  }
  return std::make_pair(SUCCESS, std::move(batch));
}

MSRStatus ShardReader::ConsumerByRow(int consumer_id) {
  // Set thread name
  auto thread_id = kThreadName + std::to_string(consumer_id);
  prctl(PR_SET_NAME, common::SafeCStr(thread_id), 0, 0, 0);

  // Loop forever
  for (;;) {
    int task_id = 0;

    // Get next task ID
    task_id = task_id_++;

    // All tasks are done
    if (task_id >= static_cast<int>(tasks_.Size())) {
      return FAILED;
    }
    const auto &ret = ConsumerOneTask(task_id, consumer_id);
    if (SUCCESS != ret.first) {
      return FAILED;
    }
    const auto &batch = ret.second;
    // Hanging if maximum map size exceeded
    //   otherwise, set batch data in map
    {
      std::unique_lock<std::mutex> lck(mtx_delivery_);
      cv_delivery_.wait(lck, [task_id, this] { return interrupt_ || task_id <= deliver_id_ + kNumBatchInMap; });
      if (interrupt_) {
        return SUCCESS;
      }
      delivery_map_[task_id] = std::make_shared<std::vector<std::tuple<std::vector<uint8_t>, json>>>(std::move(batch));
    }
    cv_iterator_.notify_one();
  }
}

MSRStatus ShardReader::ReadBlob(const int &shard_id, const uint64_t &page_offset, const int &page_length,
                                const int &buf_id) {
  auto &io_seekg = file_streams_[shard_id]->seekg(page_offset, std::ios::beg);
  if (!io_seekg.good() || io_seekg.fail() || io_seekg.bad()) {
    MS_LOG(ERROR) << "File seekg failed";
    file_streams_[shard_id]->close();
    return FAILED;
  }

  auto &io_read = file_streams_[shard_id]->read(reinterpret_cast<char *>(&buf_[buf_id][0]), page_length);
  if (!io_read.good() || io_read.fail() || io_read.bad()) {
    MS_LOG(ERROR) << "File read failed";
    file_streams_[shard_id]->close();
    return FAILED;
  }
  return SUCCESS;
}

MSRStatus ShardReader::ConsumerByBlock(int consumer_id) {
  // Set thread name
  auto thread_id = kThreadName + std::to_string(consumer_id);
  prctl(PR_SET_NAME, common::SafeCStr(thread_id), 0, 0, 0);

  // Loop forever
  for (;;) {
    int task_id = 0;

    // Get next task ID
    task_id = task_id_++;

    // All tasks are done, either quit or repeat again
    if (task_id >= num_blocks_) {
      std::unique_lock<std::mutex> lck(mtx_delivery_);
      cv_delivery_.wait(lck, [this] { return interrupt_ || task_id_ < num_blocks_; });
      if (interrupt_) {
        return SUCCESS;
      }
      continue;
    }

    // Pick up task from task list
    auto task = tasks_.get_task_by_id(tasks_.permutation_[task_id]);

    auto shard_id = std::get<0>(std::get<0>(task));
    auto group_id = std::get<1>(std::get<0>(task));
    auto row_group_brief = ReadRowGroupBrief(group_id, shard_id, selected_columns_);
    if (SUCCESS != std::get<0>(row_group_brief)) {
      return FAILED;
    }
    auto page_length = std::get<2>(row_group_brief);
    auto page_offset = std::get<3>(row_group_brief);

    MS_LOG(DEBUG) << "Block task " << task_id << tasks_.permutation_[task_id] << ", shard " << shard_id << ", group "
                  << group_id << ", page length " << page_length << ", page offset " << page_offset;

    // Deliver block data to output map
    auto offset_and_labels = std::make_pair(std::get<4>(row_group_brief), std::get<5>(row_group_brief));

    int deliver_id = deliver_id_;
    // Hanging if maximum map size exceeded otherwise, set batch data in buffer
    {
      std::unique_lock<std::mutex> lck(mtx_delivery_);
      cv_delivery_.wait(lck, [task_id, this] { return interrupt_ || task_id < deliver_id_ + kNumPageInBuffer; });
      if (interrupt_) {
        return SUCCESS;
      }
    }

    auto buf_id = task_id % kNumPageInBuffer;
    delivery_block_[buf_id] =
      std::make_shared<std::pair<std::vector<std::vector<uint64_t>>, std::vector<json>>>(offset_and_labels);

    // Read blob
    if (ReadBlob(shard_id, page_offset, page_length, buf_id) != SUCCESS) {
      return FAILED;
    }

    {
      std::unique_lock<std::mutex> lck(mtx_delivery_);
      delivery_block_set_.insert(task_id);
    }
    cv_iterator_.notify_one();
  }
}

std::shared_ptr<std::vector<std::tuple<std::vector<uint8_t>, json>>> ShardReader::GetRowFromBuffer(int buf_id,
                                                                                                   int rowId) {
  auto &blob_page = buf_[buf_id];
  auto &offsets = (*delivery_block_[buf_id]).first;
  auto &labels = (*delivery_block_[buf_id]).second;
  auto &addr_start = offsets[rowId][0];
  auto &addr_end = offsets[rowId][1];
  std::vector<uint8_t> images(blob_page.begin() + addr_start, blob_page.begin() + addr_end);
  std::vector<std::tuple<std::vector<uint8_t>, json>> batch;
  batch.emplace_back(std::move(images), std::move(labels[rowId]));
  return std::make_shared<std::vector<std::tuple<std::vector<uint8_t>, json>>>(std::move(batch));
}

std::vector<std::tuple<std::vector<uint8_t>, json>> ShardReader::GetBlockNext() {
  if (deliver_id_ >= num_blocks_) {
    return std::vector<std::tuple<std::vector<uint8_t>, json>>();
  }

  if (row_id_ == 0) {
    std::unique_lock<std::mutex> lck(mtx_delivery_);
    cv_iterator_.wait(lck, [this] { return interrupt_ || (delivery_block_set_.count(deliver_id_) > 0); });

    if (interrupt_) {
      return std::vector<std::tuple<std::vector<uint8_t>, json>>();
    }
  }
  auto buf_id = deliver_id_ % kNumPageInBuffer;
  auto res = GetRowFromBuffer(buf_id, row_id_);

  row_id_++;
  if (row_id_ == (*delivery_block_[buf_id]).first.size()) {
    row_id_ = 0;
    {
      std::unique_lock<std::mutex> lck(mtx_delivery_);
      delivery_block_set_.erase(deliver_id_++);
    }
    cv_delivery_.notify_all();
  }

  return *res;
}

std::vector<std::tuple<std::vector<uint8_t>, json>> ShardReader::GetNext() {
  if (interrupt_) {
    return std::vector<std::tuple<std::vector<uint8_t>, json>>();
  }
  if (block_reader_) return GetBlockNext();
  if (deliver_id_ >= static_cast<int>(tasks_.Size())) {
    return std::vector<std::tuple<std::vector<uint8_t>, json>>();
  }

  std::shared_ptr<std::vector<std::tuple<std::vector<uint8_t>, json>>> res;
  {
    std::unique_lock<std::mutex> lck(mtx_delivery_);
    cv_iterator_.wait(lck, [this] { return interrupt_ || (delivery_map_.count(deliver_id_) > 0); });
    if (interrupt_) {
      return std::vector<std::tuple<std::vector<uint8_t>, json>>();
    }
    res = delivery_map_[deliver_id_];
    delivery_map_.erase(deliver_id_++);
  }

  cv_delivery_.notify_all();

  return *res;
}

std::vector<std::tuple<std::vector<uint8_t>, json>> ShardReader::GetNextById(const int64_t &task_id,
                                                                             const int32_t &consumer_id) {
  if (interrupt_) {
    return std::vector<std::tuple<std::vector<uint8_t>, json>>();
  }
  if (block_reader_) {
    return GetBlockNext();
  }
  const auto &ret = ConsumerOneTask(task_id, consumer_id);
  if (SUCCESS != ret.first) {
    return std::vector<std::tuple<std::vector<uint8_t>, json>>();
  }
  return std::move(ret.second);
}

std::vector<std::tuple<std::vector<uint8_t>, pybind11::object>> ShardReader::GetNextPy() {
  auto res = GetNext();
  vector<std::tuple<std::vector<uint8_t>, pybind11::object>> jsonData;
  std::transform(res.begin(), res.end(), std::back_inserter(jsonData),
                 [](const std::tuple<std::vector<uint8_t>, json> &item) {
                   auto &j = std::get<1>(item);
                   pybind11::object obj = nlohmann::detail::FromJsonImpl(j);
                   return std::make_tuple(std::get<0>(item), std::move(obj));
                 });
  return jsonData;
}

void ShardReader::Reset() {
  {
    std::lock_guard<std::mutex> lck(mtx_delivery_);
    task_id_ = 0;
    deliver_id_ = 0;
  }
  cv_delivery_.notify_all();
}

void ShardReader::ShuffleTask() {
  for (const auto &op : operators_) {
    if (block_reader_ || !std::dynamic_pointer_cast<ShardShuffle>(op)) continue;
    if (SUCCESS != (*op)(tasks_)) {
      MS_LOG(WARNING) << "Reshuffle reader tasks failed.";
    }
  }
}

}  // namespace mindrecord
}  // namespace mindspore
