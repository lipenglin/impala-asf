// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "exec/table-sink-base.h"

#include "exec/output-partition.h"
#include "runtime/runtime-state.h"
#include "util/hdfs-util.h"
#include "util/impalad-metrics.h"
#include "util/metrics.h"
#include "util/string-util.h"
#include "util/runtime-profile-counters.h"

#include "common/names.h"

namespace impala {

Status TableSinkBase::Prepare(RuntimeState* state, MemTracker* parent_mem_tracker) {
  RETURN_IF_ERROR(DataSink::Prepare(state, parent_mem_tracker));
  partitions_created_counter_ = ADD_COUNTER(profile(), "PartitionsCreated", TUnit::UNIT);
  files_created_counter_ = ADD_COUNTER(profile(), "FilesCreated", TUnit::UNIT);
  rows_inserted_counter_ = ADD_COUNTER(profile(), "RowsInserted", TUnit::UNIT);
  bytes_written_counter_ = ADD_COUNTER(profile(), "BytesWritten", TUnit::BYTES);
  encode_timer_ = ADD_TIMER(profile(), "EncodeTimer");
  hdfs_write_timer_ = ADD_TIMER(profile(), "HdfsWriteTimer");
  compress_timer_ = ADD_TIMER(profile(), "CompressTimer");
  return Status::OK();
}

Status TableSinkBase::ClosePartitionFile(
    RuntimeState* state, OutputPartition* partition) {
  if (partition->tmp_hdfs_file == nullptr) return Status::OK();
  int hdfs_ret = hdfsCloseFile(partition->hdfs_connection, partition->tmp_hdfs_file);
  VLOG_FILE << "hdfsCloseFile() file=" << partition->current_file_name;
  partition->tmp_hdfs_file = nullptr;
  ImpaladMetrics::NUM_FILES_OPEN_FOR_INSERT->Increment(-1);
  if (hdfs_ret != 0) {
    return Status(ErrorMsg(TErrorCode::GENERAL,
        GetHdfsErrorMsg("Failed to close HDFS file: ",
        partition->current_file_name)));
  }
  return Status::OK();
}

void TableSinkBase::BuildHdfsFileNames(
    const HdfsPartitionDescriptor& partition_descriptor,
    OutputPartition* output_partition, const string &external_partition_name) {

  // Create final_hdfs_file_name_prefix and tmp_hdfs_file_name_prefix.
  // Path: <hdfs_base_dir>/<partition_values>/<unique_id_str>
  // Or, for transactional tables:
  // Path: <hdfs_base_dir>/<partition_values>/<transaction_directory>/<unique_id_str>
  // Where <transaction_directory> is either a 'base' or a 'delta' directory in Hive ACID
  // terminology.

  // Temporary files are written under the following path which is unique to this sink:
  // <table_dir>/_impala_insert_staging/<query_id>/<per_fragment_unique_id>_dir/
  // Both the temporary directory and the file name, when moved to the real partition
  // directory must be unique.
  // Prefix the directory name with "." to make it hidden and append "_dir" at the end
  // of the directory to avoid name clashes for unpartitioned tables.
  // The files are located in <partition_values>/<random_value>_data under
  // tmp_hdfs_file_name_prefix.

  // Use the query id as filename.
  const string& query_suffix = Substitute("$0_$1_data", unique_id_str_, rand());

  output_partition->tmp_hdfs_dir_name =
      Substitute("$0/.$1_$2_dir/", staging_dir(), unique_id_str_, rand());
  output_partition->tmp_hdfs_file_name_prefix = Substitute("$0$1/$2",
      output_partition->tmp_hdfs_dir_name, output_partition->partition_name,
      query_suffix);

  if (HasExternalOutputDir()) {
    // When an external FE has provided a staging directory we use that directly.
    // We are trusting that the external frontend implementation has done appropriate
    // authorization checks on the external output directory.
    output_partition->final_hdfs_file_name_prefix = Substitute("$0/$1/",
        external_output_dir_, external_partition_name);
  } else if (partition_descriptor.location().empty()) {
    output_partition->final_hdfs_file_name_prefix = Substitute("$0/$1/",
        table_desc_->hdfs_base_dir(), output_partition->partition_name);
  } else {
    // If the partition descriptor has a location (as set by alter table add partition
    // with a location clause), that provides the complete directory path for this
    // partition. No partition key suffix ("p=1/j=foo/") should be added.
    output_partition->final_hdfs_file_name_prefix =
        Substitute("$0/", partition_descriptor.location());
  }
  if (IsHiveAcid()) {
    if (HasExternalOutputDir()) {
      // The 0 padding on base and delta is to match the behavior of Hive since various
      // systems will expect a certain format for dynamic partition creation. Additionally
      // include an 0 statement id for delta directory so various Hive AcidUtils detect
      // the directory (such as AcidUtils.baseOrDeltaSubdir()). Multiple statements in a
      // single transaction is not supported.
      if (is_overwrite()) {
        output_partition->final_hdfs_file_name_prefix += StringPrintf("/base_%07ld/",
            write_id());
      } else {
        output_partition->final_hdfs_file_name_prefix += StringPrintf(
            "/delta_%07ld_%07ld_0000/", write_id(), write_id());
      }
    } else {
      string acid_dir = Substitute(
          is_overwrite() ? "/base_$0/" : "/delta_$0_$0/", write_id());
      output_partition->final_hdfs_file_name_prefix += acid_dir;
    }
  }
  if (IsIceberg()) {
    //TODO: implement LocationProviders.
    if (output_partition->partition_name.empty()) {
      output_partition->final_hdfs_file_name_prefix =
          Substitute("$0/data/", table_desc_->IcebergTableLocation());
    } else {
      output_partition->final_hdfs_file_name_prefix =
          Substitute("$0/data/$1/", table_desc_->IcebergTableLocation(),
              output_partition->partition_name);
    }
  }
  output_partition->final_hdfs_file_name_prefix += query_suffix;
  output_partition->num_files = 0;
}

Status TableSinkBase::CreateNewTmpFile(RuntimeState* state,
    OutputPartition* output_partition) {
  SCOPED_TIMER(ADD_TIMER(profile(), "TmpFileCreateTimer"));
  string file_name_pattern =
      output_partition->writer->file_extension().empty() ? "$0.$1" : "$0.$1.$2";
  string final_location = Substitute(file_name_pattern,
      output_partition->final_hdfs_file_name_prefix, output_partition->num_files,
      output_partition->writer->file_extension());

  // If ShouldSkipStaging() is true, then the table sink will write the file(s) for this
  // partition to the final location directly. If it is false, the file(s) will be written
  // to a temporary staging location which will be moved by the coordinator to the final
  // location.
  if (ShouldSkipStaging(state, output_partition)) {
    output_partition->current_file_name = final_location;
    output_partition->current_file_final_name = "";
  } else {
    output_partition->current_file_name = Substitute(file_name_pattern,
        output_partition->tmp_hdfs_file_name_prefix, output_partition->num_files,
        output_partition->writer->file_extension());
    // Save the ultimate destination for this file (it will be moved by the coordinator).
    output_partition->current_file_final_name = final_location;
  }
  // Check if tmp_hdfs_file_name exists.
  const char* tmp_hdfs_file_name_cstr =
      output_partition->current_file_name.c_str();

  if (hdfsExists(output_partition->hdfs_connection, tmp_hdfs_file_name_cstr) == 0) {
    return Status(GetHdfsErrorMsg("Temporary HDFS file already exists: ",
        output_partition->current_file_name));
  }

  // This is the block size from the HDFS partition metadata.
  uint64_t block_size = output_partition->partition_descriptor->block_size();
  // hdfsOpenFile takes a 4 byte integer as the block size.
  if (block_size > numeric_limits<int32_t>::max()) {
    return Status(Substitute("HDFS block size must be smaller than 2GB but is configured "
        "in the HDFS partition to $0.", block_size));
  }

  if (block_size == 0) block_size = output_partition->writer->default_block_size();
  if (block_size > numeric_limits<int32_t>::max()) {
    return Status(Substitute("HDFS block size must be smaller than 2GB but the target "
        "table requires $0.", block_size));
  }

  DCHECK_LE(block_size, numeric_limits<int32_t>::max());
  output_partition->tmp_hdfs_file = hdfsOpenFile(output_partition->hdfs_connection,
      tmp_hdfs_file_name_cstr, O_WRONLY, 0, 0, block_size);

  VLOG_FILE << "hdfsOpenFile() file=" << tmp_hdfs_file_name_cstr;
  if (output_partition->tmp_hdfs_file == nullptr) {
    return Status(GetHdfsErrorMsg("Failed to open HDFS file for writing: ",
        output_partition->current_file_name));
  }

  if (IsS3APath(tmp_hdfs_file_name_cstr) ||
      IsABFSPath(tmp_hdfs_file_name_cstr) ||
      IsADLSPath(tmp_hdfs_file_name_cstr) ||
      IsOSSPath(tmp_hdfs_file_name_cstr) ||
      IsGcsPath(tmp_hdfs_file_name_cstr) ||
      IsCosPath(tmp_hdfs_file_name_cstr) ||
      IsSFSPath(tmp_hdfs_file_name_cstr) ||
      IsOzonePath(tmp_hdfs_file_name_cstr)) {
    // On S3A, the file cannot be stat'ed until after it's closed, and even so, the block
    // size reported will be just the filesystem default. Similarly, the block size
    // reported for ADLS will be the filesystem default. So, remember the requested block
    // size.
    // TODO: IMPALA-9437: Ozone does not support stat'ing a file until after it's closed,
    // so for now skip the call to hdfsGetPathInfo.
    output_partition->block_size = block_size;
  } else {
    // HDFS may choose to override the block size that we've recommended, so for non-S3
    // files, we get the block size by stat-ing the file.
    hdfsFileInfo* info = hdfsGetPathInfo(output_partition->hdfs_connection,
        output_partition->current_file_name.c_str());
    if (info == nullptr) {
      return Status(GetHdfsErrorMsg("Failed to get info on temporary HDFS file: ",
          output_partition->current_file_name));
    }
    output_partition->block_size = info->mBlockSize;
    hdfsFreeFileInfo(info, 1);
  }

  ImpaladMetrics::NUM_FILES_OPEN_FOR_INSERT->Increment(1);
  COUNTER_ADD(files_created_counter_, 1);

  ++output_partition->num_files;
  output_partition->current_file_rows = 0;
  Status status = output_partition->writer->InitNewFile();
  if (!status.ok()) {
    status.MergeStatus(ClosePartitionFile(state, output_partition));
    hdfsDelete(output_partition->hdfs_connection,
        output_partition->current_file_name.c_str(), 0);
  }
  return status;
}

Status TableSinkBase::WriteRowsToPartition(
    RuntimeState* state, RowBatch* batch, PartitionPair* partition_pair) {
  // The rows of this batch may span multiple files. We repeatedly pass the row batch to
  // the writer until it sets new_file to false, indicating that all rows have been
  // written. The writer tracks where it is in the batch when it returns with new_file
  // set.
  bool new_file;
  while (true) {
    OutputPartition* output_partition = partition_pair->first.get();
    Status status =
        output_partition->writer->AppendRows(batch, partition_pair->second, &new_file);
    if (!status.ok()) {
      // IMPALA-10607: Deletes partition file if staging is skipped when appending rows
      // fails. Otherwise, it leaves the file in un-finalized state.
      if (ShouldSkipStaging(state, output_partition)) {
        status.MergeStatus(ClosePartitionFile(state, output_partition));
        hdfsDelete(output_partition->hdfs_connection,
            output_partition->current_file_name.c_str(), 0);
      }
      return status;
    }
    if (!new_file) break;
    RETURN_IF_ERROR(FinalizePartitionFile(state, output_partition));
    RETURN_IF_ERROR(CreateNewTmpFile(state, output_partition));
  }
  partition_pair->second.clear();
  return Status::OK();
}

bool TableSinkBase::ShouldSkipStaging(RuntimeState* state, OutputPartition* partition) {
  if (IsTransactional() || HasExternalOutputDir() || is_result_sink()) return true;
  // We skip staging if we are writing query results
  return (IsS3APath(partition->final_hdfs_file_name_prefix.c_str()) && !is_overwrite() &&
      state->query_options().s3_skip_insert_staging);
}

Status TableSinkBase::FinalizePartitionFile(
    RuntimeState* state, OutputPartition* partition) {
  if (partition->tmp_hdfs_file == nullptr && !is_overwrite()) return Status::OK();
  SCOPED_TIMER(ADD_TIMER(profile(), "FinalizePartitionFileTimer"));

  // OutputPartition writer could be nullptr if there is no row to output.
  if (partition->writer.get() != nullptr) {
    RETURN_IF_ERROR(partition->writer->Finalize());
    state->dml_exec_state()->UpdatePartition(
        partition->partition_name, partition->current_file_rows,
        &partition->writer->stats());
    state->dml_exec_state()->AddCreatedFile(*partition, IsIceberg(),
        partition->writer->iceberg_file_stats());
  }

  RETURN_IF_ERROR(ClosePartitionFile(state, partition));
  return Status::OK();
}

}
