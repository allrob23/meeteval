#include <vector>
#include <numeric>
#include <stdio.h>
#include <algorithm>
#include <unordered_map>
#include <map>
#include <memory>
#include <cassert>
#include <stdexcept>
#include <limits>

//#define at(x) operator[](x)

/*
 * Get the "next" index. The next index is one that is one where exactly one index is larger than the current index.
 * If the index is already the last index, it is reset to the first index.
 * Returns true if the index was advanced, false if it was reset
 */
bool advance_index(
    std::vector<unsigned int> & index,
    const std::vector<unsigned int> & dimensions,
    const unsigned int ignore_index=-1
) {
    assert(index.size() == dimensions.size());
    for (std::vector<unsigned int>::size_type d = 0; d < dimensions.size(); d++) {
        assert(index.at(d) < dimensions.at(d));
        if (d == ignore_index) continue;
        if (index.at(d) == dimensions.at(d) - 1) {
            index.at(d) = 0;
        } else {
            index.at(d)++;
            return true;
        }
    }
    return false;
}

/*
 * Represents a memory layout of a tensor when stored flattened in a vector.
 *
 * get_index() returns the (integer) index of a given (vector) index in the flattened vector.
 * advance_index() advances the index to the next index according to the dimensions of this layout. Returns true if
 *  the index was advanced, false if it was reset
 * within() checks if the given index is within the dimensions of this layout
 */
struct Layout {
    std::vector<unsigned int> strides;
    std::vector<unsigned int> dimensions;
    size_t total_size;

    unsigned int get_index(const std::vector<unsigned int> & index) const {
        assert(index.size() == dimensions.size());
        size_t i = 0;
        for (size_t d = 0; d < dimensions.size(); d++) {
            assert(index.at(d) < dimensions.at(d));
            i += index.at(d) * strides.at(d);
        }
        return i;
    }

    bool advance_index(std::vector<unsigned int> & index, const unsigned int ignore_index=-1) const {
        return ::advance_index(index, dimensions, ignore_index);
    }

    bool within(const std::vector<unsigned int> & index) const {
        assert(index.size() == dimensions.size());
        for (size_t d = 0; d < dimensions.size(); d++) {
            if (index.at(d) >= dimensions.at(d)) return false;
        }
        return true;
    }
};

/*
 * Represents a path through the reference state space
 */
struct Path {
    std::shared_ptr<Path> previous;
    unsigned int speaker;
    unsigned int utterance;
    unsigned int stream;
};

/*
 * One entry in the hypothesis state space
 *
 * cost: The cost of the path to this state
 * path: The path (in the reference space) to this state
 */
struct HypStateEntry {
    std::shared_ptr<Path> path;
    unsigned int cost;

    /*
     * Returns a new HypStateEntry with the cost incremented by the given value
     */
    HypStateEntry incremented_by(const unsigned int increment) const {
        return HypStateEntry{path, cost + increment};
    }
};

/*
 * Represents an entry state in the reference space
 *
 * cost: The cost (in the hypothesis space) of the best path to this state for that hypothesis
 * layout: The layout of `cost` in the hypothesis space
 * offset: The offset of `cost` in the hypothesis space
 */
struct StateEntry {
    std::vector<HypStateEntry> cost;
    Layout layout;
    std::vector<unsigned int> offset;
};

/*
 * Creates a `Layout` from a begin pointer and an end pointer.
 */
Layout inline make_layout(
    const std::vector<unsigned int> begin_pointer,
    const std::vector<unsigned int> end_pointer
) {
    Layout layout;
    for (size_t d = 0; d < begin_pointer.size(); d++) {
        layout.dimensions.push_back(end_pointer[d] - begin_pointer[d] + 1);
    }
    size_t j = 1;
    for (auto i : layout.dimensions) {
        layout.strides.push_back(j);

        // Check for overflow before multiplying
        if (j * i / i  != j) throw std::overflow_error("overflow_error");
        j *= i;
    }
    layout.total_size = j;
    return layout;
}

/*
 * Represents one reference utterance
 *
 * begin_time: The begin time of the utterance (earliest begin time of all words)
 * end_time: The end time of the utterance (latest end time of all words)
 * timings: The timings of all words in the utterance
 * words: The words in the utterance: TODO: is this used?
 */
struct Utterance {
    double begin_time;
    double end_time;
//    std::vector<std::pair<double, double>> timings;
//    std::vector<unsigned int> words;
};

/*
 * Bundles all timing information needed for one word
 *
 * TODO: rename members?
 */
struct Timing {
    double begin_time;
    double end_time;
    double latest_begin_time;
    double earliest_end_time;
};

bool overlaps(const Timing & a, const Timing & b) {
    return a.begin_time < b.end_time && b.begin_time < a.end_time;
}

bool overlaps2(const Timing & a, const Timing & b) {
    return a.latest_begin_time < b.earliest_end_time && b.latest_begin_time < a.earliest_end_time;
}

/*
 * Creates a std::vector<Timing> from a std::vector<std::pair<double, double>>. Copies the timings
 * into the struct and fills the latest_begin_time and earliest_end_time fields.
 */
std::vector<Timing> make_extended_timing(const std::vector<std::pair<double, double>> & timings) {
    std::vector<Timing> extended_timings(timings.size());
    if (timings.size() == 0) return extended_timings;

    double end_time = 0;
    for (size_t i = 0; i < timings.size(); i++) {
        end_time = std::max(end_time, timings.at(i).second);
        extended_timings.at(i) = {
            .begin_time=timings.at(i).first,
            .end_time=timings.at(i).second,
            // latest_begin_time is filled below
            .earliest_end_time=end_time,
        };
    }

    double begin_time = end_time;
    for (size_t i = timings.size() - 1;; --i) {
        begin_time = std::min(begin_time, timings.at(i).first);
        extended_timings.at(i).latest_begin_time = begin_time;
        if (i == 0) break;
    }
    return extended_timings;
}

struct UpdateState {
    unsigned int cost;
    unsigned int index;
};

/*
 * Computes a time-constrained update of a levenshtein row, in-place.
 *
 * TODO: do we copy too much (up, left, diagonal, ...)?
 */
void update_levenshtein_row(
    std::vector<UpdateState> &row,
    const std::vector<unsigned int> & reference,
    const std::vector<unsigned int> & hypothesis,
    const std::vector<Timing> & reference_timings,
    const std::vector<Timing> & hypothesis_timings,
    const unsigned int hypothesis_begin,
    const unsigned int hypothesis_end
) {
    assert(row.size() >= hypothesis_end - hypothesis_begin + 1);
    assert(reference.size() == reference_timings.size());
    assert(hypothesis.size() == hypothesis_timings.size());
    assert(hypothesis_end <= hypothesis.size());
    assert(hypothesis_begin <= hypothesis_end);

    unsigned int steps = hypothesis_end - hypothesis_begin;

    // These two variables track the currently "active" region in the hypothesis
    // Any word before hyp_start and after hyp_end will be ignored (as not overlapping)
    // hyp_start and hyp_end are updated in every iteration
    unsigned int hyp_start = 0;
    unsigned int hyp_end = 0;

    for (size_t reference_index = 0; reference_index < reference.size(); reference_index++) {
//        printf("reference_index %d\n", reference_index);
        auto ref_symbol = reference.at(reference_index);
        auto & ref_timing = reference_timings.at(reference_index);

        UpdateState diagonal = row[hyp_start];
        row[hyp_start].cost++;
        UpdateState left = row[hyp_start];

        unsigned int row_index = hyp_start + 1;
        bool allow_shift = true;
        for (; row_index < steps + 1; row_index++ ) {
            auto hyp_symbol = hypothesis.at(row_index + hypothesis_begin - 1);
            auto & hyp_timing = hypothesis_timings.at(row_index + hypothesis_begin - 1);
            if (allow_shift) {
                if (ref_timing.latest_begin_time > hyp_timing.earliest_end_time){
                    // We can shift the starting point forward because nothing will overlap with anything before hyp_start
                    hyp_start++;
                } else {
                    allow_shift = false;
                }
            }

            UpdateState up = row[row_index];
            if (row_index > hyp_end) {
                up.cost += reference_index;
                if (diagonal.cost < up.cost) {
                    up = diagonal;
                    up.cost++;
                }
            }

            if (hyp_timing.begin_time >= ref_timing.end_time || ref_timing.begin_time >= hyp_timing.end_time) {
                // no overlap. No substitution/correct allowed
                if (up.cost < left.cost) left = up;
                left.cost++;
            } else if (ref_symbol == hyp_symbol) {
                // Overlap & correct: Diagonal is always the best path
                left = diagonal;
            } else {
                // Overlap & incorrect: Find the best path
                if (up.cost < left.cost) {
                    left = up;
                }
                if (diagonal.cost < left.cost) {
                    left = diagonal;
                }
                left.cost++; // Cost for ins/del/sub = 1
            }

            row[row_index] = left;
            diagonal = up;

            if (allow_shift) {
                // The final value for the cell at hyp_start can only be reached by insertions (direct path down)
                // from the current row. So, we can compute the final value by adding the number of updates
                // that come after this
                row[row_index - 1].cost += reference.size() - reference_index - 1;
            }
            if (ref_timing.earliest_end_time < hyp_timing.latest_begin_time) {
                break;
            }
        }
        hyp_end = row_index;
    }

    for (; hyp_end < steps; hyp_end++) {
        // Get update from top: Initial value + reference.size() insertions
        UpdateState up = row[hyp_end + 1];
        up.cost += reference.size();

        // Get update from left: left value + 1 deletion
        UpdateState left = row[hyp_end];
        left.cost++;

        if (up.cost < left.cost) left = up;
        row[hyp_end + 1] = left;
    }
}


/**
 * Make utterances from timings.
 * utterance.begin_time is filled with the earliest begin time of all words in this
 * utterance and all following utterances.
 * utterance.end_time is filled with the latest end time of all words in this
 * utterance and all previous utterances.
 */
std::vector<Utterance> make_utterances(std::vector<std::vector<std::pair<double, double>>> & timings) {
    double end_time = 0;
    std::vector<Utterance> utterances(timings.size());
    if (timings.size() == 0) return utterances;

    for (size_t i = 0; i < timings.size(); i++) {
        // Handle case where words are not sorted by time
         double begin = std::min_element(
            timings[i].begin(),
            timings[i].end(),
            [](const std::pair<double, double> & a, const std::pair<double, double> & b) {
                return a.first < b.first;
            }
        )->first;
        end_time = std::max(std::max_element(
            timings[i].begin(),
            timings[i].end(),
            [](const std::pair<double, double> & a, const std::pair<double, double> & b) {
                return a.second < b.second;
            })->second, end_time
        );
        utterances[i] = {
            .begin_time=begin,
            .end_time=end_time,
//            .timings=timings[i],
//            .words=reference[d][i],
        };
    }

    double begin_time = end_time;
    for (size_t i = utterances.size() - 1;; --i) {
        begin_time = std::min(begin_time, utterances[i].begin_time);
        utterances[i].begin_time = begin_time;
        if (i == 0) break;
    }
    assert(utterances.size() == timings.size());
    return utterances;
}


/*
 * TODO: Utterances must be sorted for this algorithm to work!
 * TODO: All utterances must contain at least one word!
 * TODO: Must have at least one speaker and one stream!
 */
std::pair<unsigned int, std::vector<std::pair<unsigned int, unsigned int>>> time_constrained_orc_levenshtein_distance_(
    std::vector<std::vector<unsigned int>> reference,
    std::vector<std::vector<unsigned int>> hypothesis,
    std::vector<std::vector<std::pair<double, double>>> reference_timings,
    std::vector<std::vector<std::pair<double, double>>> hypothesis_timings
) {
    // Check inputs
    assert(reference.size() == reference_timings.size());
    assert(hypothesis.size() == hypothesis_timings.size());
    for (unsigned int i = 0; i < reference.size(); i++) assert(reference[i].size() == reference_timings[i].size());
    for (unsigned int i = 0; i < hypothesis.size(); i++) assert(hypothesis[i].size() == hypothesis_timings[i].size());
    size_t num_reference_speakers = reference.size();
    size_t num_hypothesis_streams = hypothesis.size();
    unsigned int num_utterances = reference.size();

    // Convert reference utterances into `Utterance` structs
    // Track as begin and end time not the actual begin and end time, but the earliest seen end time
    // and (in reverse) the latest seen begin time to support unsorted utterance begin/end times
    std::vector<Utterance> reference_utterances = make_utterances(reference_timings);

//    printf("utterances\n");
//    for (std::vector<Utterance> s : reference_utterances) {
//        printf("  "); for (Utterance u : s) printf(" (%.2f, %.2f)", u.begin_time, u.end_time); printf("\n");
//    }

    // Compute for every word in the hypothesis the earliest seen end time and the latest seen begin time
    // This is required when the words are not sorted by begin time.
//    printf("computing extended_hypothesis_timings\n");
    std::vector<std::vector<Timing>> extended_hypothesis_timings(num_hypothesis_streams);
    for (size_t s = 0; s < hypothesis.size(); s++) {
//        printf("s: %d, size: %d\n", s, hypothesis_timings.at(s).size());
        extended_hypothesis_timings.at(s) = make_extended_timing(hypothesis_timings.at(s));
    }

//    printf("computing extended_reference_timings\n");
    std::vector<std::vector<Timing>> extended_reference_timings(reference_timings.size());
    for (size_t u = 0; u < reference_timings.size(); u++) {
        extended_reference_timings.at(u) = make_extended_timing(reference_timings.at(u));
    }

    // State: We only need one state here because we only have a single speaker,
    // i.e., only a single possible path through the reference space
    StateEntry state = {
        .cost=std::vector<HypStateEntry>(1),
        .layout=make_layout(std::vector<unsigned int>(num_hypothesis_streams), std::vector<unsigned int>(num_hypothesis_streams)),
        .offset=std::vector<unsigned int>(hypothesis.size())
    };
    double ref_block_begin_time = 0;
    double ref_block_end_time = 0;

    // Hypothesis state space indices
    std::vector<unsigned int> new_state_offset(hypothesis.size());
    std::vector<unsigned int> new_state_end(hypothesis.size());
    std::vector<unsigned int> indices;

    // Pre-allocate temporary memory TODO: this is often (but not always!) larger than necessary
    unsigned int max_num_states = 0;
    for (size_t s = 0; s < hypothesis.size(); s++) max_num_states = std::max(max_num_states, (unsigned int) hypothesis.at(s).size());
    std::vector<UpdateState> tmp_row(max_num_states + 1);

    // Iterate through reference utterances in temporal order
    for (unsigned int u = 0; u < num_utterances; u++) {
        Utterance current_reference_utterance = reference_utterances.at(u);

        // Compute size of new state TODO: is this too large?
        // This state has the dimensions of the hypothesis words that overlap
        // on each stream with the current block of reference utterances
        for (size_t s = 0; s < hypothesis.size(); s++) {
            // Find which words overlap with the current reference block, i.e., which portion of the state
            // has to be computed

            while (
                new_state_offset.at(s) < hypothesis.at(s).size()
                && extended_hypothesis_timings.at(s).at(new_state_offset.at(s)).earliest_end_time <
                current_reference_utterance.begin_time
            ) {
                new_state_offset.at(s)++;
            }
//            new_state_end.at(s) = new_state_offset.at(s);
            new_state_end.at(s) = 0;
            while (
                new_state_end.at(s) < hypothesis.at(s).size()
                && extended_hypothesis_timings.at(s).at(new_state_end.at(s)).latest_begin_time <
                current_reference_utterance.end_time
            ) {
                new_state_end.at(s)++;
            }
        }

        // Compute new cells
        // We only have one possible previous state for ORC-WER! (since there
        // is only one speaker)
        Layout target_layout = make_layout(new_state_offset, new_state_end);
        auto first_update = true;
        StateEntry new_state = {
            .cost=std::vector<HypStateEntry>(target_layout.total_size),
            .layout=target_layout,
            .offset=new_state_offset,
        };
//            printf("new_state size: %d\n", new_state.cost.size());
//            printf("new_state layout: "); for (auto i : new_state.layout.dimensions) printf(" %d", i); printf("\n");

        auto & prev_state = state; // TODO: rename. We don't need two pointers to the same object

        auto &active_reference = reference.at(u);
        auto &active_reference_timings = extended_reference_timings.at(u);

        // Compute the update for every hypothesis stream and min over that
        for (unsigned int s = 0; s < hypothesis.size(); s++) {
            // TODO: only iterate over the streams that actually overlap with the current reference
            unsigned int old_distance = -1;
            // TODO: eliminate the index vector and multiplications
            std::vector<unsigned int> new_state_index(new_state.layout.dimensions.size());
            std::vector<unsigned int> old_state_index(prev_state.layout.dimensions.size());
            std::vector<unsigned int> old_closest_index(new_state_index.size());

            // Iterate over all starting points of of levenshtein rows along the active_hypothesis_index dimension,
            // i.e., where new_state_index[s] == 0
//            printf("Start loop");
            while(true) {
//                printf("while loop\n");
                // Translate the new state index into the old state index
                for (size_t i = 0; i < old_state_index.size(); i++) {
                    assert(new_state.offset.at(i) >= prev_state.offset.at(i));
                    assert(prev_state.offset.at(i) <= new_state_index.at(i) + new_state_offset.at(i));
                    old_state_index.at(i) = new_state_index.at(i) + new_state_offset.at(i) - prev_state.offset.at(i);
                }

                // Find the index in prev state that is closest to the new index
                std::vector<unsigned int> closest_index(new_state_index.size());
                unsigned int distance = 0;
                for (size_t i = 0; i < closest_index.size(); i++) {
                    closest_index[i] = std::min(
                        (unsigned int) old_state_index[i],
                        (unsigned int) prev_state.layout.dimensions[i] - 1
                    );
                    assert(old_state_index[i] >= closest_index[i]);
                    distance += old_state_index[i] - closest_index[i];
                }
                assert(prev_state.layout.within(closest_index));

                // TODO: store all of them in a hash map to further reduce the number of calls of update_levensthein_row?
                if (false && old_distance != -1 && old_closest_index == closest_index && distance > old_distance) {
                    // If the distance is greater than 0 we use an earlier state and fill with insertions.
                    // The levenshtein is invariant to a constant offset, so we can simply take the
                    // last row and add the insertions
                    for (unsigned int s_ = 0; s_ < new_state.layout.dimensions.at(s); s_++) {
                        tmp_row[s_].cost += distance - old_distance;
                    }
                } else {
                    // Fill temporary row
                    for (unsigned int s_ = 0; s_ < new_state.layout.dimensions.at(s); s_++) {
                        if (prev_state.layout.within(closest_index)) {
                            // Copy from previous state
                            // We have to add the distance because the states might not be overlapping
                            tmp_row.at(s_).index = prev_state.layout.get_index(closest_index);
                            tmp_row.at(s_).cost = prev_state.cost.at(tmp_row.at(s_).index ).cost + distance;
                        } else {
                            // Pad with insertions
                            assert(s_ > 0);
                            tmp_row.at(s_).index = tmp_row.at(s_ - 1).index;
                            tmp_row.at(s_).cost = tmp_row.at(s_ - 1).cost + 1;
                        }

                        // Increment state index to move along with s_.
                        // This might move closest_index out of the prev_state area
                        closest_index[s]++;
                    }

//                            printf("Forwarding Levenshtein row (d: %d, u: %d, s: %d, length: %d)\n", d, indices.at(d) - 1, s, new_state_end.at(s) - new_state_offset.at(s));
//                            printf("tmp_row ["); for (UpdateState i : tmp_row) printf(" %d", i.cost); printf("] -> ");
                    // Forward tmp row
                    update_levenshtein_row(
                        tmp_row,
                        active_reference,
                        hypothesis.at(s),
                        active_reference_timings,
                        extended_hypothesis_timings.at(s),
                        new_state.offset.at(s),
                        new_state_end.at(s)
                    );
                }
                old_distance = distance;
                old_closest_index = closest_index;

//                        printf("%d [", distance); for (int i =0; i < new_state_end.at(s) - new_state_offset.at(s); i++) printf(" %d", tmp_row.at(i).cost); printf("]\n");
//                        printf("["); for (UpdateState i : tmp_row) printf(" %d", i.cost); printf("]\n");
                // Copy into state
                assert(new_state_index[s] == 0);
                for (unsigned int s_ = 0; s_ < new_state.layout.dimensions.at(s); s_++) {
                    auto _index = new_state.layout.get_index(new_state_index);

                    if (first_update || tmp_row.at(s_).cost < new_state.cost[_index].cost) {
                        new_state.cost[_index].cost = tmp_row.at(s_).cost;
                        new_state.cost[_index].path = std::make_shared<struct Path>(Path{
                            .previous=prev_state.cost.at(tmp_row.at(s_).index).path,
                            .speaker=0, // TODO: this is always 0 for ORC-WER
                            .utterance=u,
                            .stream=s
                        });
                    }
                    new_state_index[s]++;
                }

                new_state_index[s] = 0;
//                printf("new_state_index:"); for (auto i : new_state_index) printf(" %d", i); printf("\n");
                if (!new_state.layout.advance_index(new_state_index, s)) break;
            }
            first_update = false;
        }
        state = new_state;
    }

    auto final_state = state;
    unsigned int cost = final_state.cost.back().cost;

    // Handle any words in the hypothesis that begin after the last reference ended
    for (size_t d = 0; d < num_hypothesis_streams; d++) {
        if (hypothesis.at(d).size() > final_state.offset.at(d) + final_state.layout.dimensions.at(d) - 1) {
            cost += hypothesis.at(d).size() - final_state.offset.at(d) - final_state.layout.dimensions.at(d) + 1;
        }
    }

    // Get assignment
    // [(reference speaker index, hypothesis stream index)]
    std::vector<std::pair<unsigned int, unsigned int>> assignment;
    Path path = *final_state.cost.back().path;

    while (true) {
        assignment.push_back({path.speaker, path.stream});
        if (path.previous) {
            path = *path.previous;
        } else {
            break;
        }
    }
    std::reverse(assignment.begin(), assignment.end());
    return std::make_pair(cost, assignment);
}