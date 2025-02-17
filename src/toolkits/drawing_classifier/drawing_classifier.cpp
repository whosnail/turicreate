/* Copyright © 2019 Apple Inc. All rights reserved.
 *
 * Use of this source code is governed by a BSD-3-clause license that can
 * be found in the LICENSE.txt file or at
 * https://opensource.org/licenses/BSD-3-Clause
 */

#include <iostream>
#include <memory>
#include <random>
#include <sstream>

#include <core/logging/assertions.hpp>
#include <core/logging/logger.hpp>
#include <core/util/string_util.hpp>
#include <ml/neural_net/compute_context.hpp>
#include <ml/neural_net/model_backend.hpp>
#include <ml/neural_net/model_spec.hpp>
#include <model_server/lib/variant_deep_serialize.hpp>
#include <toolkits/coreml_export/neural_net_models_exporter.hpp>
#include <toolkits/evaluation/metrics.hpp>
#include <toolkits/supervised_learning/automatic_model_creation.hpp>
#include <toolkits/util/training_utils.hpp>

#include <toolkits/drawing_classifier/drawing_classifier.hpp>

namespace turi {
namespace drawing_classifier {

namespace {

using coreml::MLModelWrapper;
using neural_net::compute_context;
using neural_net::float_array_map;
using neural_net::model_backend;
using neural_net::model_spec;
using neural_net::shared_float_array;
using neural_net::weight_initializer;
using neural_net::xavier_weight_initializer;
using neural_net::zero_weight_initializer;

using padding_type = model_spec::padding_type;
// anonymous helper sections

struct result {
  shared_float_array loss_info;
  shared_float_array accuracy_info;
  data_iterator::batch data_info;
};

}  // namespace

const size_t drawing_classifier::DRAWING_CLASSIFIER_VERSION = 1;

size_t drawing_classifier::get_version() const {
  return DRAWING_CLASSIFIER_VERSION;
}

void drawing_classifier::save_impl(oarchive& oarc) const {
  if (!nn_spec_)
    log_and_throw(
        "model spec is not initalized, please call `init_train` before saving model");

  // Save model attributes.
  variant_deep_save(state, oarc);

  // Save neural net weights.
  oarc << nn_spec_->export_params_view();
}

void drawing_classifier::load_version(iarchive& iarc, size_t version) {
  if (!nn_spec_)
    log_and_throw(
        "model spec is not initalized, please call `init_train` before loading model");

  // Load model attributes.
  variant_deep_load(state, iarc);

  // Load neural net weights.
  float_array_map nn_params;
  iarc >> nn_params;

  nn_spec_ = init_model();
  nn_spec_->update_params(nn_params);
}

std::unique_ptr<model_spec> drawing_classifier::init_model() const {
  std::unique_ptr<model_spec> result(new model_spec);

  // state is updated through init_train
  flex_string target = read_state<flex_string>("target");
  size_t num_classes = read_state<flex_int>("num_classes");

  // feature columns names
  const flex_list &features_list = read_state<flex_list>("features");

  result->add_channel_concat(
      "features",
      std::vector<std::string>(features_list.begin(), features_list.end()));

  std::mt19937 random_engine;
  try {
    std::seed_seq seed_seq{read_state<int>("random_seed")};
    random_engine = std::mt19937(seed_seq);
  } catch (const std::out_of_range &e) {
  }

  weight_initializer initializer = zero_weight_initializer();

  const std::string prefix{"drawing"};
  // add suffix when needed.
  const std::string _suffix{""};
  std::string input_name{"features"};
  std::string output_name;

  {
    size_t channels_filter = 16;
    size_t channels_kernel = 1;
    std::stringstream ss;

    for (size_t ii = 0; ii < 3; ii++) {
      if (ii) {
        input_name = std::move(output_name);
      }

      ss.str("");
      ss << prefix << "_conv" << ii << _suffix;
      output_name = ss.str();

      initializer = xavier_weight_initializer(
          /* #input_neurons   */ channels_kernel * 3 * 3,
          /* #output_neurons  */ channels_filter * 3 * 3, &random_engine);

      result->add_convolution(
          /* name                */ output_name,
          /* input               */ input_name,
          /* num_output_channels */ channels_filter,
          /* num_kernel_channels */ channels_kernel,
          /* kernel_height       */ 3,
          /* kernel_width        */ 3,
          /* stride_height       */ 1,
          /* stride_width        */ 1,
          /* padding             */ padding_type::SAME,
          /* weight_init_fn      */ initializer,
          /* bias_init_fn        */ zero_weight_initializer());

      channels_kernel = channels_filter;
      channels_filter *= 2;

      input_name = std::move(output_name);
      ss.str("");
      ss << prefix << "_relu" << ii << _suffix;
      output_name = ss.str();

      result->add_relu(output_name, input_name);

      input_name = std::move(output_name);
      ss.str("");
      ss << prefix << "_pool" << ii << _suffix;
      output_name = ss.str();
      result->add_pooling(
          /* name                 */ output_name,
          /* input                */ input_name,
          /* kernel_height        */ 2,
          /* kernel_width         */ 2,
          /* stride_height        */ 2,
          /* stride_width         */ 2,
          /* padding              */ padding_type::VALID,
          /* avg excluded padding */ false);
    }
  }

  input_name = std::move(output_name);
  output_name = prefix + "_flatten0" + _suffix;

  result->add_flatten(output_name, input_name);

  input_name = std::move(output_name);
  output_name = prefix + "_dense0" + _suffix;

  initializer = xavier_weight_initializer(
      /* fan_in    */ 64 * 3 * 3,
      /* fan_out   */ 128, &random_engine);

  result->add_inner_product(
      /* name                */ output_name,
      /* input               */ input_name,
      /* num_output_channels */ 128,
      /* num_input_channels  */ 64 * 3 * 3,
      /* weight_init_fn      */ initializer);

  input_name = std::move(output_name);
  output_name = prefix + "_dense1" + _suffix;

  initializer = xavier_weight_initializer(
      /* fan_in    */ 128,
      /* fan_out   */ num_classes, &random_engine);

  result->add_inner_product(
      /* name                */ output_name,
      /* input               */ input_name,
      /* num_output_channels */ num_classes,
      /* num_input_channels  */ 128,
      /* weight_init_fn      */ initializer);

  input_name = std::move(output_name);

  result->add_softmax(target + "Probability", input_name);

  return result;
}

void drawing_classifier::init_options(
    const std::map<std::string, flexible_type> &opts) {
  // Define options.
  options.create_integer_option(
      "batch_size", "Number of training examples used per training step", 256,
      1, std::numeric_limits<int>::max());
  options.create_integer_option(
      "max_iterations",
      "Maximum number of iterations/epochs made over the data during the"
      " training phase",
      500,
      1,
      std::numeric_limits<int>::max());

  // Validate user-provided options.
  options.set_options(opts);

  // Write model fields.
  add_or_update_state(flexmap_to_varmap(options.current_option_values()));
}

std::tuple<gl_sframe, gl_sframe> drawing_classifier::init_data(
    gl_sframe data, variant_type validation_data) const {
  return turi::supervised::create_validation_data(data, validation_data);
}

std::unique_ptr<data_iterator> drawing_classifier::create_iterator(
    data_iterator::parameters iterator_params) const {
  return std::unique_ptr<data_iterator>(
      new simple_data_iterator(iterator_params));
}

std::unique_ptr<data_iterator> drawing_classifier::create_iterator(
    gl_sframe data, bool is_train,
    std::vector<std::string> class_labels) const {
  data_iterator::parameters data_params;
  data_params.data = std::move(data);

  if (!is_train) {
    data_params.class_labels = std::move(class_labels);
  }

  data_params.is_train = is_train;
  data_params.target_column_name = read_state<flex_string>("target");
  data_params.feature_column_name = read_state<flex_string>("feature");
  return create_iterator(data_params);
}

void drawing_classifier::init_training(
    gl_sframe data, std::string target_column_name,
    std::string feature_column_name, variant_type validation_data,
    std::map<std::string, flexible_type> opts) {
  // Read user-specified options.
  init_options(opts);

  // Perform validation split if necessary.
  std::tie(training_data_, validation_data_) = init_data(data, validation_data);

  // Begin printing progress.
  // TODO: Make progress printing optional.
  init_table_printer(!validation_data_.empty());

  add_or_update_state(
      {{"target", target_column_name}, {"feature", feature_column_name}});

  // Bind the data to a data iterator.
  training_data_iterator_ =
      create_iterator(training_data_,
                      /* is_train */ true, /* class labels */ {});

  const std::vector<std::string> &classes =
      training_data_iterator_->class_labels();
  add_or_update_state({{"classes", flex_list(classes.begin(), classes.end())}});

  // Bind the validation data to a data iterator.
  if (!validation_data_.empty()) {
    validation_data_iterator_ =
        create_iterator(validation_data_,
                        /* is_train */ false, /* class labels */ classes);
  } else {
    validation_data_iterator_ = nullptr;
  }

  // Instantiate the compute context.
  training_compute_context_ = create_compute_context();
  if (training_compute_context_ == nullptr) {
    log_and_throw("No neural network compute context provided");
  }

  // Report to the user what GPU(s) is being used.
  std::vector<std::string> gpu_names = training_compute_context_->gpu_names();
  print_training_device(gpu_names);

  // Set additional model fields.
  add_or_update_state({
      {"num_classes", training_data_iterator_->class_labels().size()},
      {"training_iterations", 0},
  });

  // Initialize the neural net. Note that this depends on statistics computed by
  // the data iterator.
  nn_spec_ = init_model();

  // TODO: Do not hardcode values
  training_model_ = training_compute_context_->create_drawing_classifier(
      nn_spec_->export_params_view(), read_state<size_t>("batch_size"),
      read_state<size_t>("num_classes"));

  // Print the header last, after any logging triggered by initialization above.
  if (training_table_printer_) {
    training_table_printer_->print_header();
  }
}

// Returns the validation accuracy and validation loss respectively as a tuple
std::tuple<float, float> drawing_classifier::compute_validation_metrics(
    size_t num_classes, size_t batch_size) {
  float cumulative_val_loss = 0.f;
  size_t val_size = 0;
  size_t val_num_correct = 0;
  size_t val_num_samples = 0;
  validation_data_iterator_->reset();

  // To support double buffering, use a queue of pending inference results.
  std::queue<result> pending_batches;

  auto pop_until_size = [&](size_t remaining) {
    while (pending_batches.size() > remaining) {
      // Pop one batch from the queue.
      result batch = pending_batches.front();
      pending_batches.pop();

      size_t batch_num_correct = 0;
      batch_num_correct = static_cast<size_t>(*(batch.accuracy_info.data()) *
                                              batch.data_info.num_samples);
      val_num_correct += batch_num_correct;
      val_num_samples += batch.data_info.num_samples;
      float val_loss =
          std::accumulate(batch.loss_info.data(),
                          batch.loss_info.data() + batch.loss_info.size(), 0.f,
                          std::plus<float>());
      cumulative_val_loss += val_loss;
    }
  };

  while (validation_data_iterator_->has_next_batch()) {
    // Wait until we have just one asynchronous batch outstanding. The work
    // below should be concurrent with the neural net inference for that batch.
    pop_until_size(1);

    result result_batch;
    result_batch.data_info = validation_data_iterator_->next_batch(batch_size);

    // Submit the batch to the neural net model.
    std::map<std::string, shared_float_array> results =
        training_model_->predict({{"input", result_batch.data_info.drawings},
                                  {"labels", result_batch.data_info.targets}});

    result_batch.accuracy_info = results.at("accuracy");
    result_batch.loss_info = results.at("loss");
    val_size += result_batch.data_info.num_samples;

    // Add the pending result to our queue and move on to the next input batch.
    pending_batches.push(std::move(result_batch));
  }
  // Process all remaining batches.
  pop_until_size(0);
  float average_val_accuracy =
      static_cast<float>(val_num_correct) / val_num_samples;
  float average_val_loss = cumulative_val_loss / val_size;

  return std::make_tuple(average_val_accuracy, average_val_loss);
}

void drawing_classifier::iterate_training() {
  // Training must have been initialized.
  ASSERT_TRUE(training_data_iterator_ != nullptr);
  ASSERT_TRUE(training_model_ != nullptr);

  const size_t batch_size = read_state<flex_int>("batch_size");
  const size_t iteration_idx = read_state<flex_int>("training_iterations");

  float cumulative_batch_loss = 0.f;
  size_t num_batches = 0;
  size_t train_num_correct = 0;
  size_t train_num_samples = 0;
  size_t num_classes = read_state<size_t>("num_classes");

  // To support double buffering, use a queue of pending inference results.
  std::queue<result> pending_batches;

  auto pop_until_size = [&](size_t remaining) {
    while (pending_batches.size() > remaining) {
      // Pop one batch from the queue.
      result batch = pending_batches.front();
      pending_batches.pop();

      size_t batch_num_correct = 0;
      batch_num_correct = static_cast<size_t>(*(batch.accuracy_info.data()) *
                                              batch.data_info.num_samples);
      train_num_correct += batch_num_correct;
      train_num_samples += batch.data_info.num_samples;

      float batch_loss =
          std::accumulate(batch.loss_info.data(),
                          batch.loss_info.data() + batch.loss_info.size(), 0.f,
                          std::plus<float>());

      cumulative_batch_loss += batch_loss / batch.data_info.num_samples;
    }
  };

  while (training_data_iterator_->has_next_batch()) {
    // Wait until we have just one asynchronous batch outstanding. The work
    // below should be concurrent with the neural net inference for that batch.
    pop_until_size(1);

    result result_batch;
    result_batch.data_info = training_data_iterator_->next_batch(batch_size);

    // Submit the batch to the neural net model.
    std::map<std::string, shared_float_array> results =
        training_model_->train({{"input", result_batch.data_info.drawings},
                                {"labels", result_batch.data_info.targets}});
    result_batch.loss_info = results.at("loss");
    result_batch.accuracy_info = results.at("accuracy");

    ++num_batches;

    // Add the pending result to our queue and move on to the next input batch.
    pending_batches.push(std::move(result_batch));
  }
  // Process all remaining batches.
  pop_until_size(0);
  float average_batch_loss = cumulative_batch_loss / num_batches;
  float average_batch_accuracy =
      static_cast<float>(train_num_correct) / train_num_samples;
  float average_val_accuracy;
  float average_val_loss;

  if (validation_data_iterator_) {
    std::tie(average_val_accuracy, average_val_loss) =
        compute_validation_metrics(num_classes, batch_size);
  }
  add_or_update_state({
      {"training_iterations", iteration_idx + 1},
      {"training_accuracy", average_batch_accuracy},
      {"training_log_loss", average_batch_loss},
  });

  if (validation_data_iterator_) {
    add_or_update_state({
        {"validation_accuracy", average_val_accuracy},
        {"validation_log_loss", average_val_loss},
    });
  }

  if (training_table_printer_) {
    if (validation_data_iterator_) {
      training_table_printer_->print_progress_row(
          iteration_idx, iteration_idx + 1, average_batch_accuracy,
          average_batch_loss, average_val_accuracy, average_val_loss,
          progress_time());
    } else {
      training_table_printer_->print_progress_row(
          iteration_idx, iteration_idx + 1, average_batch_accuracy,
          average_batch_loss, progress_time());
    }
  }

  training_data_iterator_->reset();
}  // namespace

std::unique_ptr<compute_context> drawing_classifier::create_compute_context()
    const {
  return compute_context::create_tf();
}

void drawing_classifier::init_table_printer(bool has_validation) {
  if (has_validation) {
    training_table_printer_.reset(
        new table_printer({{"Iteration", 12},
                           {"Train Accuracy", 12},
                           {"Train Loss", 12},
                           {"Validation Accuracy", 12},
                           {"Validation Loss", 12},
                           {"Elapsed Time", 12}}));
  } else {
    training_table_printer_.reset(new table_printer({{"Iteration", 12},
                                                     {"Train Accuracy", 12},
                                                     {"Train Loss", 12},
                                                     {"Elapsed Time", 12}}));
  }
}

void drawing_classifier::train(gl_sframe data, std::string target_column_name,
                               std::string feature_column_name,
                               variant_type validation_data,
                               std::map<std::string, flexible_type> opts) {
  // Instantiate the training dependencies: data iterator, compute context,
  // backend NN model.
  init_training(data, target_column_name, feature_column_name, validation_data,
                opts);

  // Perform all the iterations at once.
  flex_int max_iterations = read_state<flex_int>("max_iterations");
  while (read_state<flex_int>("training_iterations") < max_iterations) {
    iterate_training();
  }

  // Finish printing progress.
  training_table_printer_->print_footer();
  training_table_printer_.reset();

  // Sync trained weights to our local storage of the NN weights.
  float_array_map trained_weights = training_model_->export_weights();
  nn_spec_->update_params(trained_weights);

  variant_map_type state_update;

  // Update the state with recall, precision and confusion matrix for training
  // data
  gl_sarray train_predictions = predict(training_data_, "probability_vector");
  variant_map_type train_metric = evaluation::compute_classifier_metrics(
      training_data_, target_column_name, "report", train_predictions,
      {{"classes", read_state<flex_list>("classes")}});

  for (auto &p : train_metric) {
    state_update["training_" + p.first] = p.second;
  }

  // Update the state with recall, precision and confusion matrix for validation
  // data
  if (!validation_data_.empty()) {
    gl_sarray val_predictions = predict(validation_data_, "probability_vector");
    variant_map_type val_metric = evaluation::compute_classifier_metrics(
        validation_data_, target_column_name, "report", val_predictions,
        {{"classes", read_state<flex_list>("classes")}});

    for (auto &p : val_metric) {
      state_update["validation_" + p.first] = p.second;
    }
  }

  add_or_update_state(state_update);
}

gl_sarray drawing_classifier::predict(gl_sframe data, std::string output_type) {
  /* TODO: Add code to predict! */
  return gl_sarray();
}

gl_sframe drawing_classifier::predict_topk(gl_sframe data,
                                           std::string output_type, size_t k) {
  /* TODO: Add code to predict_topk! */
  return gl_sframe();
}

variant_map_type drawing_classifier::evaluate(gl_sframe data,
                                              std::string metric) {
  // Perform prediction.
  gl_sarray predictions = predict(data, "probability_vector");

  /* TODO: This is just for the skeleton. Rewrite. */
  return evaluation::compute_classifier_metrics(data, "label", metric,
                                                predictions, {{"classes", 2}});
}

std::shared_ptr<coreml::MLModelWrapper> drawing_classifier::export_to_coreml(
    std::string filename, bool use_default_spec) {
  /* Add code for export_to_coreml */
  if (!nn_spec_) {
    // use empty nn spec if not initalized;
    // avoid test bad memory access
    if (use_default_spec) {
      nn_spec_ = std::unique_ptr<model_spec>(new model_spec);
    } else {
      log_and_throw(
          "model is not initialized; please call train before export_coreml");
    }
  }

  std::shared_ptr<MLModelWrapper> model_wrapper =
      export_drawing_classifier_model(
          *nn_spec_, read_state<flex_list>("features"),
          read_state<flex_list>("classes"), read_state<flex_string>("target"));

  const flex_list &features_list = read_state<flex_list>("features");
  const flex_string features_string =
      join(std::vector<std::string>(features_list.begin(), features_list.end()),
           ",");

  flex_dict user_defined_metadata = {
      {"target", read_state<flex_string>("target")},
      {"features", features_string},
      {"max_iterations", read_state<flex_int>("max_iterations")},
      {"warm_start", read_state<flex_int>("warm_start")},
      {"type", "drawing_classifier"},
      {"version", 2},
  };

  model_wrapper->add_metadata(
      {{"user_defined", std::move(user_defined_metadata)}});

  if (!filename.empty()) {
    model_wrapper->save(filename);
  }

  return model_wrapper;
}

}  // namespace drawing_classifier
}  // namespace turi
