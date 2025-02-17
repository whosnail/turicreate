/* Copyright © 2019 Apple Inc. All rights reserved.
 *
 * Use of this source code is governed by a BSD-3-clause license that can
 * be found in the LICENSE.txt file or at https://opensource.org/licenses/BSD-3-Clause
 */

#define BOOST_TEST_MODULE test_dc_data_iterator

#include <toolkits/drawing_classifier/dc_data_iterator.hpp>

#include <algorithm>

#include <boost/test/unit_test.hpp>
#include <model_server/lib/image_util.hpp>
#include <core/util/test_macros.hpp>

#include "data_utils.hpp"

namespace turi {
namespace drawing_classifier {

const std::vector<std::string> unique_labels = {"foo", "bar", "baz"};

/** Runs all standard tests for a simple_data_iterator
 * 
 * Parameters
 * ----------
 *
 * params : data_iterator::parameters
 *
 * num_rows : size_t
 *
 * batch_size : size_t
 *
 * checked_class_labels : bool
 * A flag to indicate whether expected_class_labels were passed in when params
 * were generated. If true, class labels are assumed to have tested outside of
 * this function. If false, class labels are tested in this function.
 *
 */
void test_simple_data_iterator_with_num_rows_and_batch_size(
  const drawing_data_generator &data_generator,
  size_t num_rows, size_t batch_size, bool checked_class_labels) {
  data_iterator::parameters params = data_generator.get_iterator_params();
  TS_ASSERT_EQUALS(params.data.size(), num_rows);
  /* Create a simple data iterator */
  simple_data_iterator data_source(params);
  std::vector<std::string> actual_class_labels = data_source.class_labels();
  std::unordered_map<std::string, int> class_to_index_map = data_source.class_to_index_map();
  /* Test class labels */
  if (!checked_class_labels) {
    /* expected_class_labels were not passed in to the params, so we need to 
     * make sure the inferred class labels are correct
     */
    std::vector<std::string> expected_class_labels = data_generator.get_unique_labels();
    TS_ASSERT_EQUALS(actual_class_labels.size(), expected_class_labels.size());
    for (size_t i = 0; i < actual_class_labels.size(); i++) {
      TS_ASSERT_EQUALS(actual_class_labels[i], expected_class_labels[i]);
    }
  }
  /* Call next_batch */
  data_iterator::batch next_batch = data_source.next_batch(batch_size);
  /* Test drawing and target sizes */
  TS_ASSERT_EQUALS(next_batch.drawings.size(),
                   batch_size * IMAGE_WIDTH * IMAGE_HEIGHT * 1);
  TS_ASSERT_EQUALS(next_batch.targets.size(), batch_size);
  /* Test drawing shape */
  size_t actual_dim = next_batch.drawings.dim();
  size_t expected_dim = 4;
  TS_ASSERT_EQUALS(actual_dim, expected_dim);
  size_t expected_shape[4] = {batch_size, IMAGE_WIDTH, IMAGE_HEIGHT, 1};
  const size_t *actual_shape = next_batch.drawings.shape();
  for (size_t i=0; i<actual_dim; i++) {
    TS_ASSERT_EQUALS(actual_shape[i], expected_shape[i]);
  }
  gl_sframe data = params.data;
  size_t index_in_data = 0;
  /* Test target contents */
  const float *actual_target_data = next_batch.targets.data();
  for (size_t index_in_batch = 0; index_in_batch < batch_size; index_in_batch++) {
    float expected_target = static_cast<float>(
      class_to_index_map[
        data[params.target_column_name][index_in_data].to<flex_string>()
      ]);
    float actual_target = actual_target_data[index_in_batch];
    TS_ASSERT_EQUALS(expected_target, actual_target);
    index_in_data = (index_in_data + 1) % data.size();
  }
  /* Test drawing contents */
  index_in_data = 0;
  const float *actual_drawing_data = next_batch.drawings.data();
  for (size_t index_in_batch = 0; index_in_batch < batch_size; index_in_batch++) {
    flex_image decoded_drawing = image_util::decode_image(
    data[params.feature_column_name][index_in_data].to<flex_image>());
    const unsigned char *expected_drawing_data = decoded_drawing.get_image_data();
    for (size_t row = 0; row < IMAGE_HEIGHT; row++) {
      for (size_t col = 0; col < IMAGE_WIDTH; col++) {
        // Asserting that the (row, col) index of every drawing in the batch
        // matches the (row, col) index we have in the original SFrame.
        TS_ASSERT_EQUALS(
          static_cast<unsigned char>(actual_drawing_data[
            index_in_batch * IMAGE_WIDTH * IMAGE_HEIGHT * 1
            + row * IMAGE_WIDTH * 1
            + col * 1]),
          expected_drawing_data[row * IMAGE_WIDTH + col]
          );
      }
    }
    index_in_data = (index_in_data + 1) % data.size();
  }
}

BOOST_AUTO_TEST_CASE(test_simple_data_iterator) {
  static constexpr size_t MAX_NUM_ROWS = 4;
  static constexpr size_t MAX_BATCH_SIZE = 8;
  for (size_t num_rows = 1; num_rows <= MAX_NUM_ROWS; num_rows++) {
    for (size_t batch_size = 1; batch_size <= MAX_BATCH_SIZE; batch_size++) {
      drawing_data_generator data_generator(num_rows, unique_labels);
      data_iterator::parameters params = data_generator.get_iterator_params();
  
      test_simple_data_iterator_with_num_rows_and_batch_size(data_generator, num_rows,
        batch_size, false);
    }
  }
}

BOOST_AUTO_TEST_CASE(test_simple_data_iterator_with_expected_class_labels) {
  static constexpr size_t NUM_ROWS = 1;
  static constexpr size_t BATCH_SIZE = 10;
  drawing_data_generator data_generator(NUM_ROWS, unique_labels);
  std::vector<std::string> class_labels = { "bar", "foo" }; 
  // Purposely added an extraneous label here.
  data_generator.set_class_labels(class_labels);
  data_iterator::parameters params = data_generator.get_iterator_params();
  simple_data_iterator data_source(params);
  TS_ASSERT_EQUALS(data_source.class_labels(), class_labels);
  // Confirm that the extraneous label appears in the data_source class_labels.
  test_simple_data_iterator_with_num_rows_and_batch_size(
    data_generator, NUM_ROWS, BATCH_SIZE, true);
}

BOOST_AUTO_TEST_CASE(test_simple_data_iterator_with_unexpected_classes) {

  static constexpr size_t NUM_ROWS = 1;

  drawing_data_generator data_generator(NUM_ROWS, unique_labels);
  data_iterator::parameters params = data_generator.get_iterator_params();
  
  params.class_labels = { "bad_class" };

  // The data contains the label "foo", which is not among the expected class
  // labels.
  TS_ASSERT_THROWS_ANYTHING(simple_data_iterator unused_var(params));
}

/* TODO: Add a test to test multiple calls to next_batch on the same dataset */


}  // namespace drawing_classifier
}  // namespace turi