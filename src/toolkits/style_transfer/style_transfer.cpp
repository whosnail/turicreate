/* Copyright © 2019 Apple Inc. All rights reserved.
 *
 * Use of this source code is governed by a BSD-3-clause license that can
 * be found in the LICENSE.txt file or at
 * https://opensource.org/licenses/BSD-3-Clause
 */

#include <toolkits/style_transfer/style_transfer.hpp>

#include <map>
#include <random>
#include <string>

#include <core/data/image/image_type.hpp>
#include <model_server/lib/image_util.hpp>
#include <model_server/lib/variant_deep_serialize.hpp>
#include <toolkits/style_transfer/style_transfer_model_definition.hpp>

namespace turi {
namespace style_transfer {

using turi::coreml::MLModelWrapper;
using turi::neural_net::compute_context;
using turi::neural_net::float_array_map;
using turi::neural_net::float_scalar;
using turi::neural_net::shared_float_array;

namespace {

constexpr size_t STYLE_TRANSFER_VERSION = 1;

constexpr size_t DEFAULT_HEIGHT = 256;

constexpr size_t DEFAULT_WIDTH = 256;

constexpr size_t DEFAULT_BATCH_SIZE = 1;

void prepare_images(const image_type& image,
                    std::vector<float>::iterator start_iter, size_t width,
                    size_t height, size_t channels, size_t index) {
  size_t image_size = height * width * channels;

  image_type resized_image =
      image_util::resize_image(image, width, height, channels, true, 1);

  const unsigned char* resized_image_ptr = resized_image.get_image_data();

  std::transform(resized_image_ptr, resized_image_ptr + image_size, start_iter,
                 [](unsigned char val) { return val / 255.f; });
}

float_array_map prepare_batch(std::vector<st_example>& batch, size_t width,
                              size_t height) {
  constexpr size_t channels = 3;
  const size_t batch_size = batch.size();

  std::vector<float> content_array(height * width * channels * batch.size());
  std::vector<float> style_array(height * width * channels * batch.size());
  std::vector<float> index_array(batch_size);

  for (size_t index = 0; index < batch_size; index++) {
    size_t offset = index * height * width * channels;

    std::vector<float>::iterator content_iter = content_array.begin() + offset;
    prepare_images(batch[index].content_image, content_iter, width, height,
                   channels, index);

    std::vector<float>::iterator style_iter = style_array.begin() + offset;
    prepare_images(batch[index].style_image, style_iter, width, height,
                   channels, index);

    size_t style_index = batch[index].style_index;
    index_array[index] = style_index;
  }

  return {{"input",
           shared_float_array::wrap(std::move(content_array),
                                    {batch_size, height, width, channels})},
          {"labels",
           shared_float_array::wrap(std::move(style_array),
                                    {batch_size, height, width, channels})},
          {"index",
           shared_float_array::wrap(std::move(index_array), {batch_size})}};
}

flex_int estimate_max_iterations(flex_int num_styles, flex_int batch_size) {
  return static_cast<flex_int>(num_styles * 10000.0f / batch_size);
}

}  // namespace

void style_transfer::init_options(
    const std::map<std::string, flexible_type>& opts) {
  options.create_integer_option(
      "batch_size",
      "The number of images to process for each training iteration",
      FLEX_UNDEFINED, 1, std::numeric_limits<int>::max());

  options.create_integer_option(
      "max_iterations",
      "Maximum number of iterations to perform during training", FLEX_UNDEFINED,
      1, std::numeric_limits<int>::max());

  options.create_integer_option(
      "image_width", "The width of the images passed into the network",
      FLEX_UNDEFINED, 1, std::numeric_limits<int>::max());

  options.create_integer_option(
      "image_height", "The height of the images passed into the network",
      FLEX_UNDEFINED, 1, std::numeric_limits<int>::max());

  options.create_integer_option(
      "random_seed",
      "Seed for random weight initialization and sampling during training",
      FLEX_UNDEFINED, std::numeric_limits<int>::min(),
      std::numeric_limits<int>::max());

  options.create_integer_option(
      "num_styles", "The number of styles present in the model", FLEX_UNDEFINED,
      1, std::numeric_limits<int>::max());

  options.set_options(opts);

  add_or_update_state(flexmap_to_varmap(options.current_option_values()));
}

size_t style_transfer::get_version() const { return STYLE_TRANSFER_VERSION; }

void style_transfer::save_impl(oarchive& oarc) const {
  variant_deep_save(state, oarc);
  oarc << m_resnet_spec->export_params_view();
}

void style_transfer::load_version(iarchive& iarc, size_t version) {
  variant_deep_load(state, iarc);

  float_array_map nn_params;
  iarc >> nn_params;

  m_resnet_spec =
      init_resnet(variant_get_value<size_t>(state.at("num_styles")));
  m_resnet_spec->update_params(nn_params);
}

std::unique_ptr<compute_context> style_transfer::create_compute_context()
    const {
  return compute_context::create();
}

std::unique_ptr<data_iterator> style_transfer::create_iterator(
    gl_sarray style, gl_sarray content, bool repeat, int random_seed) const {
  data_iterator::parameters iterator_params;

  iterator_params.style = std::move(style);
  iterator_params.content = std::move(content);
  iterator_params.repeat = repeat;
  iterator_params.random_seed = random_seed;

  return create_iterator(iterator_params);
}

std::unique_ptr<data_iterator> style_transfer::create_iterator(
    data_iterator::parameters iterator_params) const {
  return std::unique_ptr<data_iterator>(
      new style_transfer_data_iterator(iterator_params));
}

void style_transfer::infer_derived_options() {
  if (read_state<flexible_type>("batch_size") == FLEX_UNDEFINED) {
    add_or_update_state({{"batch_size", DEFAULT_BATCH_SIZE}});
  }

  if (read_state<flexible_type>("max_iterations") == FLEX_UNDEFINED) {
    flex_int max_iterations = estimate_max_iterations(
        read_state<flex_int>("num_styles"), read_state<flex_int>("batch_size"));

    add_or_update_state({{"max_iterations", max_iterations}});
  }

  if (read_state<flexible_type>("image_width") == FLEX_UNDEFINED) {
    add_or_update_state({{"image_width", DEFAULT_WIDTH}});
  }

  if (read_state<flexible_type>("image_height") == FLEX_UNDEFINED) {
    add_or_update_state({{"image_height", DEFAULT_HEIGHT}});
  }

  add_or_update_state({{"training_iterations", 0}});
}

void style_transfer::init_train(gl_sarray style, gl_sarray content,
                                std::map<std::string, flexible_type> opts) {
  auto resnet_mlmodel_path_iter = opts.find("resnet_mlmodel_path");
  if (resnet_mlmodel_path_iter == opts.end()) {
    log_and_throw("Expected option \"resnet_mlmodel_path\" not found.");
  }
  const std::string resnet_mlmodel_path = resnet_mlmodel_path_iter->second;
  opts.erase(resnet_mlmodel_path_iter);

  auto vgg_mlmodel_path_iter = opts.find("vgg_mlmodel_path");
  if (vgg_mlmodel_path_iter == opts.end()) {
    log_and_throw("Expected option \"vgg_mlmodel_path\" not found.");
  }
  const std::string vgg_mlmodel_path = vgg_mlmodel_path_iter->second;
  opts.erase(vgg_mlmodel_path_iter);

  auto num_styles_iter = opts.find("num_styles");
  if (num_styles_iter == opts.end()) {
    log_and_throw("Expected option \"num_styles\" not found.");
  }
  size_t num_styles = num_styles_iter->second;

  init_options(opts);

  if (read_state<flexible_type>("random_seed") == FLEX_UNDEFINED) {
    std::random_device random_device;
    int random_seed = static_cast<int>(random_device());
    add_or_update_state({{"random_seed", random_seed}});
  }

  m_training_data_iterator = create_iterator(style, content, true, num_styles);

  m_training_compute_context = create_compute_context();
  if (m_training_compute_context == nullptr) {
    log_and_throw("No neural network compute context provided");
  }

  infer_derived_options();

  add_or_update_state({{"model", "resnet-16"}});

  m_resnet_spec = init_resnet(resnet_mlmodel_path, num_styles);
  m_vgg_spec = init_vgg_16(vgg_mlmodel_path);

  float_array_map weight_params = m_resnet_spec->export_params_view();
  float_array_map vgg_params = m_vgg_spec->export_params_view();

  weight_params.insert(vgg_params.begin(), vgg_params.end());

  shared_float_array st_num_styles(std::make_shared<float_scalar>(num_styles));

  m_training_model = m_training_compute_context->create_style_transfer(
      {{"st_num_styles", st_num_styles}}, weight_params);

  // TODO: print table printer
}

flex_int style_transfer::get_max_iterations() const {
  return read_state<flex_int>("max_iterations");
}

flex_int style_transfer::get_training_iterations() const {
  return read_state<flex_int>("training_iterations");
}

flex_int style_transfer::get_num_classes() const {
  return read_state<flex_int>("num_classes");
}

void style_transfer::iterate_training() {
  ASSERT_TRUE(m_training_data_iterator != nullptr);
  ASSERT_TRUE(m_training_model != nullptr);

  flex_int iteration_idx = get_training_iterations();

  flex_int batch_size = read_state<flex_int>("batch_size");
  flex_int image_width = read_state<flex_int>("image_width");
  flex_int image_height = read_state<flex_int>("image_height");

  std::vector<st_example> batch =
      m_training_data_iterator->next_batch(batch_size);

  float_array_map prepared_batch =
      prepare_batch(batch, image_width, image_height);

  std::map<std::string, shared_float_array> results =
      m_training_model->train(prepared_batch);

  add_or_update_state({
      {"training_iterations", iteration_idx + 1},
  });

  shared_float_array loss_batch = results.at("loss");

  size_t loss_batch_size = loss_batch.size();
  float batch_loss = std::accumulate(
      loss_batch.data(), loss_batch.data() + loss_batch_size, 0.f,
      [loss_batch_size](float a, float b) { return a + b / loss_batch_size; });

  if (training_table_printer_) {
    training_table_printer_->print_progress_row(
        iteration_idx, iteration_idx + 1, batch_loss, progress_time());
  }
}

void style_transfer::finalize_training() {
  float_array_map trained_weights = m_training_model->export_weights();
  m_resnet_spec->update_params(trained_weights);
}

void style_transfer::train(gl_sarray style, gl_sarray content,
                           std::map<std::string, flexible_type> opts) {
  training_table_printer_.reset(new table_printer(
      {{"Iteration", 12}, {"Loss", 12}, {"Elapsed Time", 12}}));

  init_train(style, content, opts);

  training_table_printer_->print_header();

  while (get_training_iterations() < get_max_iterations()) iterate_training();

  finalize_training();

  training_table_printer_->print_footer();
  training_table_printer_.reset();
}

std::shared_ptr<MLModelWrapper> style_transfer::export_to_coreml(
    std::string filename, std::map<std::string, flexible_type> opts) {
  flex_int image_width = read_state<flex_int>("image_width");
  flex_int image_height = read_state<flex_int>("image_height");

  flex_dict user_defined_metadata = {
      {"model", read_state<flex_string>("model")},
      {"max_iterations", read_state<flex_int>("max_iterations")},
      {"training_iterations", read_state<flex_int>("training_iterations")},
      {"type", "StyleTransfer"},
      {"content_feature", "image"},
      {"style_feature", "image"},
      {"num_styles", read_state<flex_string>("num_styles")},
      {"version", get_version()},
  };

  std::shared_ptr<MLModelWrapper> model_wrapper =
      export_style_transfer_model(*m_resnet_spec, image_width, image_height,
                                  std::move(user_defined_metadata));

  if (!filename.empty()) model_wrapper->save(filename);

  return model_wrapper;
}

}  // namespace style_transfer
}  // namespace turi