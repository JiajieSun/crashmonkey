#include <list>
#include <vector>

#include "Permuter.h"
#include "../utils/utils.h"

namespace fs_testing {
namespace permuter {

using std::list;
using std::size_t;
using std::vector;

using fs_testing::utils::disk_write;

namespace {

struct range {
  unsigned int start;
  unsigned int end;
};

static const unsigned int kRetryMultiplier = 2;
static const unsigned int kMinRetries = 1000;

}  // namespace


size_t BioVectorHash::operator() (const vector<unsigned int>& permutation)
    const {
  unsigned int seed = permutation.size();
  for (const auto& bio_pos : permutation) {
    seed ^= bio_pos + 0x9e3779b9 + (seed << 6) + (seed >> 2);
  }
  return seed;
}

bool BioVectorEqual::operator() (const std::vector<unsigned int>& a,
    const std::vector<unsigned int>& b) const {
  if (a.size() != b.size()) {
    return false;
  }
  for (unsigned int i = 0; i < a.size(); ++i) {
    if (a.at(i) != b.at(i)) {
      return false;
    }
  }
  return true;
}

void Permuter::InitDataVector(vector<disk_write> *data) {
  epochs_.clear();
  bool prev_epoch_flush_op = false;
  disk_write data_half;
  list<range> overlaps;
  // Make sure that the first time we mark a checkpoint epoch, we start at 0 and
  // not 1.
  int curr_checkpoint_epoch = -1;
  // Aligns with the index of the bio in the profile dump, 0 indexed.
  unsigned int abs_index = 0;

  auto curr_op = data->begin();
  while (curr_op != data->end()) {
    struct epoch current_epoch;
    current_epoch.has_barrier = false;
    current_epoch.overlaps = false;
    current_epoch.checkpoint_epoch = curr_checkpoint_epoch;

    // Get all ops in this epoch and add them to either the sync_op or async_op
    // lists.
    while (curr_op != data->end() && !curr_op->is_barrier_write()) {
      // Checkpoint operations will only be seen once we have switched over
      // epochs, so we need to edit the checkpoint epoch of the current epoch as
      // well as incrementing the curr_checkpoint_epoch counter.
      if (curr_op->is_checkpoint()) {
        ++curr_checkpoint_epoch;
        current_epoch.checkpoint_epoch = curr_checkpoint_epoch;
        // Checkpoint operations should not appear in the bio stream passed to
        // actual permuters.
        ++curr_op;
        ++abs_index;
        continue;
      }
      
      if (prev_epoch_flush_op == true) {
        epoch_op split_op = {abs_index, data_half};
        // TODO(ashmrtn): Find a better way to handle matching an index to a bio
        // in the profile dump.
        //++abs_index;
        current_epoch.ops.push_back(split_op);
        current_epoch.num_meta += data_half.is_meta();
        prev_epoch_flush_op = false;
      }

      // Find overlapping ranges.
      unsigned int start = curr_op->metadata.write_sector;
      unsigned int end = start + curr_op->metadata.size;
      for (auto range_iter = overlaps.begin(); range_iter != overlaps.end();
          range_iter++) {
        range r = *range_iter;
        if ((r.start <= start && r.end >= start)
            || (r.start <= end && r.end >= end)) {
          if (r.start > start) {
            r.start = start;
          }
          if (r.end < end) {
            r.end = end;
          }
          current_epoch.overlaps = true;
          break;
        } else if (r.start > end) {
          // Since this is an ordered list, if the next spot is past where we're
          // looking now then we won't find anything past here. We may as well
          // insert an item here.
          range new_range = {start, end};
          overlaps.insert(range_iter, new_range);
        }
      }

      epoch_op eo = {abs_index, *curr_op};
      current_epoch.ops.push_back(eo);
      current_epoch.num_meta += curr_op->is_meta();
      ++abs_index;
      ++curr_op;
    }

    // Check is the op at the current index is a "barrier." If it is then add it
    // to the special spot in the epoch, otherwise just push the current epoch
    // onto the list and move to the next segment of the log.
    if (curr_op != data->end() && curr_op->is_barrier_write()) {
      // Check if the op at the current index has a flush flag with data. It it
      // has, then divide it into two halves and make the data available only in
      // the start of the next epoch. If the op has a FUA flag, then it gets
      // normally added into the current epoch.
      if ((curr_op->has_flush_flag() || curr_op->has_flush_seq_flag())
          && curr_op->has_write_flag() && (!curr_op->has_FUA_flag())) {
        disk_write flag_half;
        data_half = *curr_op;
        
        if(curr_op->has_flush_flag()) {
          flag_half.set_flush_flag();
          data_half.clear_flush_flag();
        }
        
        if(curr_op->has_flush_seq_flag()) {
          flag_half.set_flush_seq_flag();
          data_half.clear_flush_seq_flag();
        }
        
        epoch_op oe = {abs_index, flag_half};
        current_epoch.ops.push_back(oe);
        current_epoch.num_meta += flag_half.is_meta();
        epochs_.push_back(current_epoch);
        prev_epoch_flush_op = true;
        current_epoch.has_barrier = true;
        ++abs_index;
        ++curr_op;
        continue;
      }

      epoch_op oe = {abs_index, *curr_op};
      current_epoch.ops.push_back(oe);
      current_epoch.num_meta += curr_op->is_meta();
      current_epoch.has_barrier = true;
      ++abs_index;
      ++curr_op;
    }
    epochs_.push_back(current_epoch);
  }
}

vector<epoch>* Permuter::GetEpochs() {
  return &epochs_;
}


bool Permuter::GenerateCrashState(vector<disk_write>& res,
    PermuteTestResult &log_data) {
  vector<epoch_op> crash_state;
  unsigned long retries = 0;
  unsigned int exists = 0;
  bool new_state = true;
  vector<unsigned int> crash_state_hash;

  unsigned long max_retries =
    ((kRetryMultiplier * completed_permutations_.size()) < kMinRetries)
      ? kMinRetries
      : kRetryMultiplier * completed_permutations_.size();
  do {
    new_state = gen_one_state(crash_state, log_data);

    crash_state_hash.clear();
    crash_state_hash.resize(crash_state.size());
    for (unsigned int i = 0; i < crash_state.size(); ++i) {
      crash_state_hash.at(i) = crash_state.at(i).abs_index;
    }

    ++retries;
    exists = completed_permutations_.count(crash_state_hash);
    if (!new_state || retries >= max_retries) {
      // We've likely found all possible crash states so just break. The
      // constant in the multiplier was randomly chosen in the hopes that it
      // would be a good hueristic. This is more to make sure that we don't spin
      // endlessly than it is for it to be a good way to break out of trying to
      // make unique permutations.
      break;
    }
  } while (exists > 0);

  // Move the permuted crash state data over into the returned crash state
  // vector.
  res.clear();
  res.resize(crash_state.size());
  for (unsigned int i = 0; i < crash_state.size(); ++i) {
    res.at(i) = crash_state.at(i).op;
  }

  if (exists == 0) {
    completed_permutations_.insert(crash_state_hash);
    // We broke out of the above loop because this state is unique.
    return new_state;
  }

  // We broke out of the above loop because we haven't found a new state in some
  // time.
  return false;
}

}  // namespace permuter
}  // namespace fs_testing
