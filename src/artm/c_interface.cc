// Copyright 2014, Additive Regularization of Topic Models.

#include "artm/c_interface.h"

#include <string>
#include <iostream>  // NOLINT

#include "boost/thread/tss.hpp"
#include "boost/thread/thread.hpp"
#include "boost/date_time/posix_time/posix_time.hpp"

#include "glog/logging.h"

#include "artm/messages.pb.h"
#include "artm/score_calculator_interface.h"
#include "artm/core/common.h"
#include "artm/core/exceptions.h"
#include "artm/core/helpers.h"
#include "artm/core/master_component.h"
#include "artm/core/template_manager.h"
#include "artm/core/collection_parser.h"
#include "artm/core/batch_manager.h"

typedef artm::core::TemplateManager<std::shared_ptr< ::artm::core::MasterComponent>> MasterComponentManager;
typedef artm::core::TemplateManager<std::shared_ptr< ::artm::core::BatchManager>> AsyncProcessBatchesManager;

// Never use the following variables explicitly (only through the corresponding methods).
// It might be good idea to make them a private members of a new singleton class.
static boost::thread_specific_ptr<std::string> last_message_;
static boost::thread_specific_ptr<std::string> last_message_ex_;
static boost::thread_specific_ptr<std::string> last_error_;

static std::string* last_message() {
  if (last_message_.get() == nullptr) {
    last_message_.reset(new std::string());
  }

  return last_message_.get();
}

static std::string* last_message_ex() {
  if (last_message_ex_.get() == nullptr) {
    last_message_ex_.reset(new std::string());
  }

  return last_message_ex_.get();
}

static void set_last_error(const std::string& error) {
  if (last_error_.get() == nullptr) {
    last_error_.reset(new std::string());
  }

  last_error_->assign(error);
}

static void EnableLogging() {
  static bool logging_enabled = false;
  if (!logging_enabled) {
    FLAGS_log_dir = ".";
    FLAGS_logbufsecs = 0;
    try {
      ::google::InitGoogleLogging(".");
      ::google::SetStderrLogging(google::GLOG_WARNING);
      logging_enabled = true;
    }
    catch (...) {
      std::cerr << "InitGoogleLogging() failed.\n";
    }
  }
}

static std::shared_ptr< ::artm::core::MasterComponent> master_component(int master_id) {
  auto master_component = MasterComponentManager::singleton().Get(master_id);
  if (master_component == nullptr) {
    BOOST_THROW_EXCEPTION(::artm::core::InvalidMasterIdException(boost::lexical_cast<std::string>(master_id)));
  }

  return master_component;
}

static char* StringAsArray(std::string* str) {
  return str->empty() ? NULL : &*str->begin();
}

static void ParseFromArray(const char* buffer, int length, google::protobuf::Message* message) {
  if (!message->ParseFromArray(buffer, length))
    BOOST_THROW_EXCEPTION(::artm::core::CorruptedMessageException("Unable to parse the message"));
}

// =========================================================================
// Common routines
// =========================================================================

int ArtmCopyRequestResult(int length, char* address) {
  ::artm::CopyRequestResultArgs args;
  std::string blob = args.SerializeAsString();
  return ArtmCopyRequestResultEx(length, address, blob.size(), blob.c_str());
}

int ArtmCopyRequestResultEx(int length, char* address, int args_length, const char* copy_result_args) {
  try {
    ::artm::CopyRequestResultArgs args;
    ParseFromArray(copy_result_args, args_length, &args);

    std::string* source = nullptr;
    if (args.request_type() == artm::CopyRequestResultArgs_RequestType_DefaultRequestType) source = last_message();
    if (args.request_type() == artm::CopyRequestResultArgs_RequestType_GetThetaSecondPass) source = last_message_ex();
    if (args.request_type() == artm::CopyRequestResultArgs_RequestType_GetModelSecondPass) source = last_message_ex();

    if (source == nullptr) {
      std::stringstream ss;
      ss << "CopyRequestResultArgs.request_type=" << args.request_type()  << " is not valid.";
      set_last_error(ss.str().c_str());
      return ARTM_INVALID_OPERATION;
    }

    if (length != static_cast<int>(source->size())) {
      std::stringstream ss;
      ss << "ArtmCopyRequestResultEx() called with invalid 'length' parameter ";
      ss << "(" << source->size() << " expected, found " << length << ").";
      set_last_error(ss.str());
      return ARTM_INVALID_OPERATION;
    }

    memcpy(address, StringAsArray(source), length);
    LOG(INFO) << "ArtmCopyRequestResultEx(request_type=" << args.request_type() << ") copied " << length << " bytes";

    return ARTM_SUCCESS;
  } CATCH_EXCEPTIONS;
}

const char* ArtmGetLastErrorMessage() {
  if (last_error_.get() == nullptr) {
    return nullptr;
  }

  return last_error_->c_str();
}

int ArtmSaveBatch(const char* disk_path, int length, const char* batch) {
  try {
    EnableLogging();
    artm::Batch batch_object;
    ParseFromArray(batch, length, &batch_object);
    artm::core::Helpers::FixAndValidate(&batch_object);
    artm::Batch compacted_batch;
    artm::core::BatchHelpers::CompactBatch(batch_object, &compacted_batch);
    artm::core::BatchHelpers::SaveBatch(compacted_batch, std::string(disk_path));
    return ARTM_SUCCESS;
  } CATCH_EXCEPTIONS;
}

int ArtmAddBatch(int master_id, int length, const char* add_batch_args) {
  try {
    artm::AddBatchArgs args;
    ParseFromArray(add_batch_args, length, &args);
    if (args.has_batch()) artm::core::Helpers::FixAndValidate(args.mutable_batch());
    bool result = master_component(master_id)->AddBatch(args);
    if (result) {
      return ARTM_SUCCESS;
    } else {
      set_last_error("Artm's processor queue is full. Call ArtmAddBatch() later.");
      return ARTM_STILL_WORKING;
    }
  } CATCH_EXCEPTIONS;
}

int ArtmInvokeIteration(int master_id, int length, const char* invoke_iteration_args) {
  try {
    artm::InvokeIterationArgs invoke_iteration_args_object;
    ParseFromArray(invoke_iteration_args, length, &invoke_iteration_args_object);
    master_component(master_id)->InvokeIteration(invoke_iteration_args_object);
    return ARTM_SUCCESS;
  } CATCH_EXCEPTIONS;
}

int ArtmWaitIdle(int master_id, int length, const char* wait_idle_args) {
  try {
    artm::WaitIdleArgs wait_idle_args_object;
    ParseFromArray(wait_idle_args, length, &wait_idle_args_object);
    bool result = master_component(master_id)->WaitIdle(wait_idle_args_object);

    if (result) {
      return ARTM_SUCCESS;
    } else {
      set_last_error("Artm is still processing the collection. Call ArtmWaitIdle() later.");
      return ARTM_STILL_WORKING;
    }
  } CATCH_EXCEPTIONS;
}

// =========================================================================
// MasterComponent
// =========================================================================

int ArtmCreateMasterComponent(int length, const char* master_component_config) {
  try {
    EnableLogging();

    artm::MasterComponentConfig config;
    ParseFromArray(master_component_config, length, &config);
    ::artm::core::Helpers::FixAndValidate(&config, /* throw_error =*/ true);
    auto& mcm = MasterComponentManager::singleton();
    int retval = mcm.Store(std::make_shared< ::artm::core::MasterComponent>(config));
    LOG(INFO) << "Creating MasterComponent (id=" << retval << ")...";
    return retval;
  } CATCH_EXCEPTIONS;
}

int ArtmDuplicateMasterComponent(int master_id, int length, const char* duplicate_master_args) {
  try {
    EnableLogging();

    std::shared_ptr< ::artm::core::MasterComponent> master = master_component(master_id);
    auto& mcm = MasterComponentManager::singleton();
    int retval = mcm.Store(master->Duplicate());
    LOG(INFO) << "Copying MasterComponent (id=" << master_id << " to id=" << retval << ")...";
    return retval;
  } CATCH_EXCEPTIONS;
}

int ArtmCreateModel(int master_id, int length, const char* model_config) {
  return ArtmReconfigureModel(master_id, length, model_config);
}

int ArtmReconfigureMasterComponent(int master_id, int length, const char* master_component_config) {
  try {
    artm::MasterComponentConfig config;
    ParseFromArray(master_component_config, length, &config);
    ::artm::core::Helpers::FixAndValidate(&config, /* throw_error =*/ true);
    master_component(master_id)->Reconfigure(config);
    return ARTM_SUCCESS;
  } CATCH_EXCEPTIONS;
}

int ArtmReconfigureModel(int master_id, int length, const char* model_config) {
  try {
    artm::ModelConfig config;
    ParseFromArray(model_config, length, &config);
    ::artm::core::Helpers::FixAndValidate(&config, /* throw_error =*/ true);
    master_component(master_id)->CreateOrReconfigureModel(config);
    return ARTM_SUCCESS;
  } CATCH_EXCEPTIONS;
}

int ArtmAsyncProcessBatches(int master_id, int length, const char* process_batches_args) {
  try {
    artm::ProcessBatchesArgs args;
    ParseFromArray(process_batches_args, length, &args);
    ::artm::core::Helpers::FixAndValidate(&args, /* throw_error =*/ true);
    std::shared_ptr< ::artm::core::MasterComponent> master = master_component(master_id);

    std::shared_ptr< ::artm::core::BatchManager> batch_manager = std::make_shared< ::artm::core::BatchManager>();
    master->AsyncRequestProcessBatches(args, batch_manager.get());
    int retval = AsyncProcessBatchesManager::singleton().Store(batch_manager);

    LOG(INFO) << "Creating async operation (id=" << retval << ")...";
    return retval;
  } CATCH_EXCEPTIONS;
}

int ArtmAwaitOperation(int operation_id, int length, const char* await_operation_args) {
  try {
    artm::AwaitOperationArgs args;
    ParseFromArray(await_operation_args, length, &args);

    AsyncProcessBatchesManager& manager = AsyncProcessBatchesManager::singleton();
    std::shared_ptr<artm::core::BatchManager> batch_manager = manager.Get(operation_id);

    const int timeout = args.timeout_milliseconds();
    auto time_start = boost::posix_time::microsec_clock::local_time();
    for (;;) {
      if (batch_manager->IsEverythingProcessed())
        return ARTM_SUCCESS;

      boost::this_thread::sleep(boost::posix_time::milliseconds(::artm::core::kIdleLoopFrequency));
      auto time_end = boost::posix_time::microsec_clock::local_time();
      if (timeout >= 0) {
        if ((time_end - time_start).total_milliseconds() >= timeout)
          break;
      }
    }

    set_last_error("The operation is still in progress. Call ArtmAwaitOperation() later.");
    return ARTM_STILL_WORKING;
  } CATCH_EXCEPTIONS;
}

int ArtmMergeModel(int master_id, int length, const char* merge_model_args) {
  try {
    artm::MergeModelArgs args;
    ParseFromArray(merge_model_args, length, &args);
    master_component(master_id)->MergeModel(args);
    return ARTM_SUCCESS;
  } CATCH_EXCEPTIONS;
}

int ArtmRegularizeModel(int master_id, int length, const char* regularize_model_args) {
  try {
    artm::RegularizeModelArgs args;
    ParseFromArray(regularize_model_args, length, &args);
    master_component(master_id)->RegularizeModel(args);
    return ARTM_SUCCESS;
  } CATCH_EXCEPTIONS;
}

int ArtmNormalizeModel(int master_id, int length, const char* normalize_model_args) {
  try {
    artm::NormalizeModelArgs args;
    ParseFromArray(normalize_model_args, length, &args);
    master_component(master_id)->NormalizeModel(args);
    return ARTM_SUCCESS;
  } CATCH_EXCEPTIONS;
}

int ArtmRequestRegularizerState(int master_id, const char* regularizer_name) {
  try {
    artm::RegularizerInternalState regularizer_state;
    master_component(master_id)->RequestRegularizerState(regularizer_name, &regularizer_state);
    regularizer_state.SerializeToString(last_message());
    return last_message()->size();
  } CATCH_EXCEPTIONS;
}

int ArtmOverwriteTopicModel(int master_id, int length, const char* topic_model) {
  try {
    artm::TopicModel topic_model_object;
    ParseFromArray(topic_model, length, &topic_model_object);
    ::artm::core::Helpers::FixAndValidate(&topic_model_object, /* throw_error =*/ true);
    master_component(master_id)->OverwriteTopicModel(topic_model_object);
    return ARTM_SUCCESS;
  } CATCH_EXCEPTIONS;
}

int ArtmInitializeModel(int master_id, int length, const char* init_model_args) {
  try {
    artm::InitializeModelArgs args;
    ParseFromArray(init_model_args, length, &args);
    ::artm::core::Helpers::FixAndValidate(&args, /* throw_error =*/ true);
    master_component(master_id)->InitializeModel(args);
    return ARTM_SUCCESS;
  } CATCH_EXCEPTIONS;
}

int ArtmExportModel(int master_id, int length, const char* export_model_args) {
  try {
    artm::ExportModelArgs args;
    ParseFromArray(export_model_args, length, &args);
    ::artm::core::Helpers::Validate(args, /* throw_error =*/ true);
    master_component(master_id)->ExportModel(args);
    return ARTM_SUCCESS;
  } CATCH_EXCEPTIONS;
}

int ArtmImportModel(int master_id, int length, const char* init_model_args) {
  try {
    artm::ImportModelArgs args;
    ParseFromArray(init_model_args, length, &args);
    ::artm::core::Helpers::Validate(args, /* throw_error =*/ true);
    master_component(master_id)->ImportModel(args);
    return ARTM_SUCCESS;
  } CATCH_EXCEPTIONS;
}

int ArtmAttachModel(int master_id, int length, const char* attach_model_args, int address_length, char* address) {
  try {
    artm::AttachModelArgs args;
    ParseFromArray(attach_model_args, length, &args);
    master_component(master_id)->AttachModel(args, address_length, reinterpret_cast<float *>(address));
    return ARTM_SUCCESS;
  } CATCH_EXCEPTIONS;
}

int ArtmDisposeMasterComponent(int master_id) {
  try {
    MasterComponentManager::singleton().Erase(master_id);
    LOG(INFO) << "Disposing MasterComponent (id=" << master_id << ")...";
    return ARTM_SUCCESS;
  } CATCH_EXCEPTIONS;
}

int ArtmDisposeModel(int master_id, const char* model_name) {
  try {
    master_component(master_id)->DisposeModel(model_name);
    return ARTM_SUCCESS;
  } CATCH_EXCEPTIONS;
}

int ArtmCreateRegularizer(int master_id, int length, const char* regularizer_config) {
  return ArtmReconfigureRegularizer(master_id, length, regularizer_config);
}

int ArtmReconfigureRegularizer(int master_id, int length, const char* regularizer_config) {
  try {
    ::artm::RegularizerConfig config;
    ParseFromArray(regularizer_config, length, &config);
    master_component(master_id)->CreateOrReconfigureRegularizer(config);
    return ARTM_SUCCESS;
  } CATCH_EXCEPTIONS;
}

int ArtmDisposeRegularizer(int master_id, const char* regularizer_name) {
  try {
    master_component(master_id)->DisposeRegularizer(regularizer_name);
    return ARTM_SUCCESS;
  } CATCH_EXCEPTIONS;
}

int ArtmSynchronizeModel(int master_id, int length, const char* sync_model_args) {
  try {
    ::artm::SynchronizeModelArgs args;
    ParseFromArray(sync_model_args, length, &args);
    master_component(master_id)->SynchronizeModel(args);
    return ARTM_SUCCESS;
  } CATCH_EXCEPTIONS;
}

int ArtmGatherDictionary(int master_id, int length, const char* gather_dictionary_args) {
  // this method creates DictionaryData and than Dictionary, based on it
  // the method DOESN't store DictionaryData into disk, it's the task of ExportDictionary
  try {
    artm::GatherDictionaryArgs args;
    ParseFromArray(gather_dictionary_args, length, &args);
    ::artm::core::Helpers::Validate(args, /* throw_error =*/ true);
    master_component(master_id)->GatherDictionary(args);
    return ARTM_SUCCESS;
  } CATCH_EXCEPTIONS;
}

int ArtmFilterDictionary(int master_id, int length, const char* filter_dictionary_config) {
  try {
    artm::FilterDictionaryArgs args;
    ParseFromArray(filter_dictionary_config, length, &args);
    ::artm::core::Helpers::FixAndValidate(&args, /* throw_error =*/ true);
    master_component(master_id)->FilterDictionary(args);
    return ARTM_SUCCESS;
  } CATCH_EXCEPTIONS;
}

int ArtmCreateDictionary(int master_id, int length, const char* dictionary_data) {
  try {
    artm::DictionaryData message;
    ParseFromArray(dictionary_data, length, &message);
    ::artm::core::Helpers::FixAndValidate(&message, /* throw_error =*/ true);
    master_component(master_id)->CreateDictionary(message);
    return ARTM_SUCCESS;
  } CATCH_EXCEPTIONS;
}

int ArtmDisposeDictionary(int master_id, const char* dictionary_name) {
  try {
    master_component(master_id)->DisposeDictionary(dictionary_name);
    return ARTM_SUCCESS;
  } CATCH_EXCEPTIONS;
}

int ArtmImportDictionary(int master_id, int length, const char* import_dictionary_args) {
  try {
    artm::ImportDictionaryArgs args;
    ParseFromArray(import_dictionary_args, length, &args);
    ::artm::core::Helpers::Validate(args, /* throw_error =*/ true);
    master_component(master_id)->ImportDictionary(args);
    return ARTM_SUCCESS;
  } CATCH_EXCEPTIONS;
}

int ArtmExportDictionary(int master_id, int length, const char* export_dictionary_args) {
  try {
    artm::ExportDictionaryArgs args;
    ParseFromArray(export_dictionary_args, length, &args);
    master_component(master_id)->ExportDictionary(args);
    return ARTM_SUCCESS;
  } CATCH_EXCEPTIONS;
}

int ArtmImportBatches(int master_id, int length, const char* import_batches_args) {
  try {
    artm::ImportBatchesArgs args;
    ParseFromArray(import_batches_args, length, &args);
    master_component(master_id)->ImportBatches(args);
    return ARTM_SUCCESS;
  } CATCH_EXCEPTIONS;
}

int ArtmDisposeBatch(int master_id, const char* batch_name) {
  try {
    master_component(master_id)->DisposeBatch(batch_name);
    return ARTM_SUCCESS;
  } CATCH_EXCEPTIONS;
}

int ArtmParseCollection(int length, const char* collection_parser_config) {
  try {
    EnableLogging();
    artm::CollectionParserConfig config;
    ParseFromArray(collection_parser_config, length, &config);
    ::artm::core::Helpers::Validate(config, /* throw_error =*/ true);
    ::artm::core::CollectionParser collection_parser(config);
    collection_parser.Parse();
    return ARTM_SUCCESS;
  } CATCH_EXCEPTIONS;
}

int ArtmRequestLoadBatch(const char* filename) {
  try {
    EnableLogging();
    auto batch = std::make_shared< ::artm::Batch>();
    ::artm::core::BatchHelpers::LoadMessage(filename, batch.get());
    batch->SerializeToString(last_message());
    return last_message()->size();
  } CATCH_EXCEPTIONS;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// EXECUTE functionality
///////////////////////////////////////////////////////////////////////////////////////////////////


///////////////////////////////////////////////////////////////////////////////////////////////////
// REQUEST functionality
///////////////////////////////////////////////////////////////////////////////////////////////////

template<typename ArgsT, typename ResultT>
int ArtmRequest(int master_id, int length, const char* args_blob) {
  try {
    ArgsT args;
    ResultT result;
    ParseFromArray(args_blob, length, &args);
    ::artm::core::Helpers::FixAndValidate(&args, /* throw_error =*/ true);
    master_component(master_id)->Request(args, &result);
    result.SerializeToString(last_message());
    return last_message()->size();
  } CATCH_EXCEPTIONS;
}

template<typename ArgsT, typename ResultT>
int ArtmRequestExternal(int master_id, int length, const char* args_blob) {
  try {
    ArgsT args;
    ResultT result;
    ParseFromArray(args_blob, length, &args);
    ::artm::core::Helpers::FixAndValidate(&args, /* throw_error =*/ true);
    master_component(master_id)->Request(args, &result, last_message_ex());
    result.SerializeToString(last_message());
    return last_message()->size();
  } CATCH_EXCEPTIONS;
}

int ArtmRequestScore(int master_id, int length, const char* args) {
  return ArtmRequest< ::artm::GetScoreValueArgs,
                      ::artm::ScoreData>(master_id, length, args);
}

int ArtmRequestDictionary(int master_id, int length, const char* args) {
  return ArtmRequest< ::artm::GetDictionaryArgs,
                      ::artm::DictionaryData>(master_id, length, args);
}

int ArtmRequestMasterComponentInfo(int master_id, int length, const char* args) {
  return ArtmRequest< ::artm::GetMasterComponentInfoArgs,
                      ::artm::MasterComponentInfo>(master_id, length, args);
}

int ArtmRequestProcessBatches(int master_id, int length, const char* args) {
  return ArtmRequest< ::artm::ProcessBatchesArgs,
                      ::artm::ProcessBatchesResult>(master_id, length, args);
}

int ArtmRequestProcessBatchesExternal(int master_id, int length, const char* args) {
  return ArtmRequestExternal< ::artm::ProcessBatchesArgs,
                              ::artm::ProcessBatchesResult>(master_id, length, args);
}

int ArtmRequestThetaMatrix(int master_id, int length, const char* args) {
  return ArtmRequest< ::artm::GetThetaMatrixArgs,
                      ::artm::ThetaMatrix>(master_id, length, args);
}

int ArtmRequestThetaMatrixExternal(int master_id, int length, const char* args) {
  return ArtmRequestExternal< ::artm::GetThetaMatrixArgs,
                              ::artm::ThetaMatrix>(master_id, length, args);
}

int ArtmRequestTopicModel(int master_id, int length, const char* args) {
  return ArtmRequest< ::artm::GetTopicModelArgs,
                      ::artm::TopicModel>(master_id, length, args);
}

int ArtmRequestTopicModelExternal(int master_id, int length, const char* args) {
  return ArtmRequestExternal< ::artm::GetTopicModelArgs,
                              ::artm::TopicModel>(master_id, length, args);
}
