// DO NOT EDIT;
// Generated by ml/nn/runtime/test/specs/generate_test.sh
#include "../../TestGenerated.h"

namespace space_to_batch_float_1 {
std::vector<MixedTypedExample> examples = {
// Generated space_to_batch_float_1 test
#include "generated/examples/space_to_batch_float_1.example.cpp"
};
// Generated model constructor
#include "generated/models/space_to_batch_float_1.model.cpp"
} // namespace space_to_batch_float_1
TEST_F(GeneratedTests, space_to_batch_float_1) {
    execute(space_to_batch_float_1::CreateModel,
            space_to_batch_float_1::is_ignored,
            space_to_batch_float_1::examples);
}
