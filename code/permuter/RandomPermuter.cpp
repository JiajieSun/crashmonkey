#include <iterator>
#include <list>
#include <numeric>
#include <vector>
#include <algorithm>
#include <cassert>

#include "Permuter.h"
#include "RandomPermuter.h"

namespace fs_testing {
namespace permuter {
using std::advance;
using std::iota;
using std::list;
using std::mt19937;
using std::uniform_int_distribution;
using std::vector;

using fs_testing::utils::disk_write;

GenRandom::GenRandom() : rand(mt19937(42)) { }

int GenRandom::operator()(int max) {
  uniform_int_distribution<unsigned int> uid(0, max - 1);
  return uid(rand);
}

RandomPermuter::RandomPermuter(vector<disk_write> *data) {
  // TODO(ashmrtn): Make a flag to make it random or not.
  rand = mt19937(42);
}

void RandomPermuter::init_data(vector<epoch> *data) {
}

bool RandomPermuter::gen_one_state(vector<epoch_op>& res,
    PermuteTestResult &log_data) {
  // Return if there are no ops to permute and generate a crash state
  if (GetEpochs()->size() == 0) {
    return false;
  }
  unsigned int total_elements = 0;
  // Find how many elements we will be returning (randomly determined).
  uniform_int_distribution<unsigned int> permute_epochs(1, GetEpochs()->size());
  unsigned int num_epochs = permute_epochs(rand);
  // Don't subtract 1 from this size so that we can send a complete epoch if we
  // want.
  uniform_int_distribution<unsigned int> permute_requests(1,
      GetEpochs()->at(num_epochs - 1).ops.size());
  unsigned int num_requests = permute_requests(rand);
  // If the last epoch has zero ops, we set num_requests to zero instead of a 
  // garbage value returned by permute_requests(rand)
  if (GetEpochs()->at(num_epochs - 1).ops.size() == 0) {
    num_requests = 0;
  }
  for (unsigned int i = 0; i < num_epochs - 1; ++i) {
    total_elements += GetEpochs()->at(i).ops.size();
  }
  total_elements += num_requests;
  res.resize(total_elements);
  log_data.crash_state.resize(total_elements);
  // Tell CrashMonkey the most recently seen checkpoint for the crash state
  // we're generating. We can't just pull the last epoch because it could be the
  // case that there's a checkpoint at the end of this disk write epoch.
  // Therefore, we should determine 1. if we are writing out this entire epoch,
  // and 2. if the checkpoint epoch for this disk write epoch is different than
  // the checkpoint epoch of the disk write epoch before this one (indicating
  // that this disk write epoch changes checkpoint epochs).
  epoch *target = &GetEpochs()->at(num_epochs - 1);
  epoch *prev = NULL;
  if (num_epochs > 1) {
    prev = &GetEpochs()->at(num_epochs - 2);
  }
  if (num_requests != target->ops.size()) {
    log_data.last_checkpoint = (prev) ? prev->checkpoint_epoch : 0;
  } else {
    log_data.last_checkpoint = target->checkpoint_epoch;
  }

  auto curr_iter = res.begin();
  for (unsigned int i = 0; i < num_epochs; ++i) {

    // We donot want to modify epochs prior to the one we are crashing at.
    // We will drop a subset of bios in the epoch we are currently crashing at
    // Only if num_req < ops in the target epoch, we need to pick a subset, else 
    // we'll just copy all the bios in this epoch
    if (i == num_epochs - 1 && num_requests < target->ops.size()) {

      unsigned int size = num_requests;
      auto res_end = curr_iter + size;

      //This should drop a subset of bios instead of permuting them
      subset_epoch(curr_iter, res_end, GetEpochs()->at(i));

      curr_iter = res_end;
    } else {
      // Use a for loop since vector::insert inserts new elements and we
      // resized above to the exact size we will have.
      // We will only ever be placing the full epoch here because the above if
      // will catch the case where we place only part of an epoch.
      for (auto epoch_iter = GetEpochs()->at(i).ops.begin();
          epoch_iter != GetEpochs()->at(i).ops.end(); ++epoch_iter) {
        *curr_iter = *epoch_iter;
        ++curr_iter;
      }
    }
  }

  // Messy bit to add everything to the logging data struct.
  for (unsigned int i = 0; i < res.size(); ++i) {
    log_data.crash_state.at(i) = res.at(i).abs_index;
  }
  return true;
}


void RandomPermuter::subset_epoch(
      vector<epoch_op>::iterator &res_start,
      vector<epoch_op>::iterator &res_end,
      epoch &epoch) {

  unsigned int req_size = distance(res_start, res_end);
  assert(req_size <= epoch.ops.size());

  // Even if the number of bios we're placing is less than the number in the
  // epoch, allow any bio but the barrier (if present) to be picked.
  unsigned int slots = epoch.ops.size();
  if (epoch.has_barrier) {
    --slots;
  }

  // Bitmap to indicate which bios in the epoch we want to pick for the crash
  // state.
  vector<unsigned int> epoch_op_bitmap(epoch.ops.size());

  // Fill the list with the empty slots, either [0, epoch.size() - 1] or
  // [0, epoch.size() - 2]. Prefer a list so that removals are fast. We have
  // this so that each time we shuffle the indexes of bios in the epoch, and
  // pick the first req_size number of bios.

  vector<unsigned int> indices(slots);
  iota(indices.begin(), indices.end(), 0);
  // Use a known random generator function for repeatability.
  std::random_shuffle(indices.begin(), indices.end(), subset_random_);

  // Populate the bitmap to set req_set number of bios.
  for (int i = 0; i < req_size; i++) {
    epoch_op_bitmap[indices[i]] = 1;
  }

  // Return the bios corresponding to bitmap indexes.
  for (unsigned int filled = 0;
      filled < epoch_op_bitmap.size() && res_start != res_end; filled++) {
    if (epoch_op_bitmap[filled] == 1) {
      *res_start = epoch.ops.at(filled);
      ++res_start;
    }
  }

  // We are only placing part of an epoch so we need to return here.
  if (res_start == res_end) {
    return;
  }

  assert(epoch.has_barrier);

  // Place the barrier operation if it exists since the entire vector already
  // exists (i.e. we won't cause extra shifting when adding the other elements).
  // Decrement out count of empty slots since we have filled one.
  *res_start = epoch.ops.back();
}


void RandomPermuter::permute_epoch(
      vector<epoch_op>::iterator& res_start,
      vector<epoch_op>::iterator& res_end,
      epoch& epoch) {
  assert(distance(res_start, res_end) <= epoch.ops.size());

  // Even if the number of bios we're placing is less than the number in the
  // epoch, allow any bio but the barrier (if present) to be picked.
  unsigned int slots = epoch.ops.size();
  if (epoch.has_barrier) {
    --slots;
  }

  // Fill the list with the empty slots, either [0, epoch.size() - 1] or
  // [0, epoch.size() - 2]. Prefer a list so that removals are fast. We have
  // this so that each time we pick a number we can find a bio which we haven't
  // already placed.
  list<unsigned int> empty_slots(slots);
  iota(empty_slots.begin(), empty_slots.end(), 0);

  // First case is when we are placing a subset of the bios, the second is when
  // we are placing all the bios but a barrier operation is present.
  while (res_start != res_end && !empty_slots.empty()) {
    // Uniform distribution includes both ends, so we need to subtract 1 from
    // the size.
    uniform_int_distribution<unsigned int> uid(0, empty_slots.size() - 1);
    auto shift = empty_slots.begin();
    advance(shift, uid(rand));
    *res_start = epoch.ops.at(*shift);
    ++res_start;
    empty_slots.erase(shift);
  }

  // We are only placing part of an epoch so we need to return here.
  if (res_start == res_end) {
    return;
  }

  assert(epoch.has_barrier);

  // Place the barrier operation if it exists since the entire vector already
  // exists (i.e. we won't cause extra shifting when adding the other elements).
  // Decrement out count of empty slots since we have filled one.
  *res_start = epoch.ops.back();
}

}  // namespace permuter
}  // namespace fs_testing

extern "C" fs_testing::permuter::Permuter* permuter_get_instance(
    std::vector<fs_testing::utils::disk_write> *data) {
  return new fs_testing::permuter::RandomPermuter(data);
}

extern "C" void permuter_delete_instance(fs_testing::permuter::Permuter* p) {
  delete p;
}
