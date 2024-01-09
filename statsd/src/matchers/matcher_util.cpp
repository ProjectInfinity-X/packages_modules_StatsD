/*
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#define STATSD_DEBUG false  // STOPSHIP if true
#include "Log.h"

#include "matchers/matcher_util.h"

#include <fnmatch.h>

#include "matchers/AtomMatchingTracker.h"
#include "src/statsd_config.pb.h"
#include "stats_util.h"

using std::set;
using std::string;
using std::vector;

namespace android {
namespace os {
namespace statsd {

bool combinationMatch(const vector<int>& children, const LogicalOperation& operation,
                      const vector<MatchingState>& matcherResults) {
    bool matched;
    switch (operation) {
        case LogicalOperation::AND: {
            matched = true;
            for (const int childIndex : children) {
                if (matcherResults[childIndex] != MatchingState::kMatched) {
                    matched = false;
                    break;
                }
            }
            break;
        }
        case LogicalOperation::OR: {
            matched = false;
            for (const int childIndex : children) {
                if (matcherResults[childIndex] == MatchingState::kMatched) {
                    matched = true;
                    break;
                }
            }
            break;
        }
        case LogicalOperation::NOT:
            matched = matcherResults[children[0]] == MatchingState::kNotMatched;
            break;
        case LogicalOperation::NAND:
            matched = false;
            for (const int childIndex : children) {
                if (matcherResults[childIndex] != MatchingState::kMatched) {
                    matched = true;
                    break;
                }
            }
            break;
        case LogicalOperation::NOR:
            matched = true;
            for (const int childIndex : children) {
                if (matcherResults[childIndex] == MatchingState::kMatched) {
                    matched = false;
                    break;
                }
            }
            break;
        case LogicalOperation::LOGICAL_OPERATION_UNSPECIFIED:
            matched = false;
            break;
    }
    return matched;
}

static bool tryMatchString(const sp<UidMap>& uidMap, const FieldValue& fieldValue,
                           const string& str_match) {
    if (isAttributionUidField(fieldValue) || isUidField(fieldValue)) {
        int uid = fieldValue.mValue.int_value;
        auto aidIt = UidMap::sAidToUidMapping.find(str_match);
        if (aidIt != UidMap::sAidToUidMapping.end()) {
            return ((int)aidIt->second) == uid;
        }
        std::set<string> packageNames = uidMap->getAppNamesFromUid(uid, true /* normalize*/);
        return packageNames.find(str_match) != packageNames.end();
    } else if (fieldValue.mValue.getType() == STRING) {
        return fieldValue.mValue.str_value == str_match;
    }
    return false;
}

static bool tryMatchWildcardString(const sp<UidMap>& uidMap, const FieldValue& fieldValue,
                                   const string& wildcardPattern) {
    if (isAttributionUidField(fieldValue) || isUidField(fieldValue)) {
        int uid = fieldValue.mValue.int_value;
        // TODO(b/236886985): replace aid/uid mapping with efficient bidirectional container
        // AidToUidMapping will never have uids above 10000
        if (uid < 10000) {
            for (auto aidIt = UidMap::sAidToUidMapping.begin();
                 aidIt != UidMap::sAidToUidMapping.end(); ++aidIt) {
                if ((int)aidIt->second == uid) {
                    // Assumes there is only one aid mapping for each uid
                    return fnmatch(wildcardPattern.c_str(), aidIt->first.c_str(), 0) == 0;
                }
            }
        }
        std::set<string> packageNames = uidMap->getAppNamesFromUid(uid, true /* normalize*/);
        for (const auto& packageName : packageNames) {
            if (fnmatch(wildcardPattern.c_str(), packageName.c_str(), 0) == 0) {
                return true;
            }
        }
    } else if (fieldValue.mValue.getType() == STRING) {
        return fnmatch(wildcardPattern.c_str(), fieldValue.mValue.str_value.c_str(), 0) == 0;
    }
    return false;
}

static pair<int, int> getStartEndAtDepth(int targetField, int start, int end, int depth,
                                         const vector<FieldValue>& values) {
    // Filter by entry field first
    int newStart = -1;
    int newEnd = end;
    // because the fields are naturally sorted in the DFS order. we can safely
    // break when pos is larger than the one we are searching for.
    for (int i = start; i < end; i++) {
        int pos = values[i].mField.getPosAtDepth(depth);
        if (pos == targetField) {
            if (newStart == -1) {
                newStart = i;
            }
            newEnd = i + 1;
        } else if (pos > targetField) {
            break;
        }
    }

    return {newStart, newEnd};
}

/*
 * Returns pairs of start-end indices in vector<FieldValue> that pariticipate in matching.
 * The returned vector is empty if an error was encountered.
 * If Position is ANY and value_matcher is matches_tuple, the vector contains a start/end pair
 * corresponding for each child FieldValueMatcher in matches_tuple. For all other cases, the
 * returned vector is of size 1.
 *
 * Also updates the depth reference parameter if matcher has Position specified.
 */
static vector<pair<int, int>> computeRanges(const FieldValueMatcher& matcher,
                                            const vector<FieldValue>& values, int start, int end,
                                            int& depth) {
    // Now we have zoomed in to a new range
    std::tie(start, end) = getStartEndAtDepth(matcher.field(), start, end, depth, values);

    if (start == -1) {
        // No such field found.
        return {};
    }

    vector<pair<int, int>> ranges;
    if (matcher.has_position()) {
        // Repeated fields position is stored as a node in the path.
        depth++;
        if (depth > 2) {
            return ranges;
        }
        switch (matcher.position()) {
            case Position::FIRST: {
                for (int i = start; i < end; i++) {
                    int pos = values[i].mField.getPosAtDepth(depth);
                    if (pos != 1) {
                        // Again, the log elements are stored in sorted order. so
                        // once the position is > 1, we break;
                        end = i;
                        break;
                    }
                }
                ranges.push_back(std::make_pair(start, end));
                break;
            }
            case Position::LAST: {
                // move the starting index to the first LAST field at the depth.
                for (int i = start; i < end; i++) {
                    if (values[i].mField.isLastPos(depth)) {
                        start = i;
                        break;
                    }
                }
                ranges.push_back(std::make_pair(start, end));
                break;
            }
            case Position::ANY: {
                if (matcher.value_matcher_case() == FieldValueMatcher::kMatchesTuple) {
                    // For ANY with matches_tuple, if all the children matchers match in any of the
                    // sub trees, it's a match.
                    // Here start is guaranteed to be a valid index.
                    int currentPos = values[start].mField.getPosAtDepth(depth);
                    // Now find all sub trees ranges.
                    for (int i = start; i < end; i++) {
                        int newPos = values[i].mField.getPosAtDepth(depth);
                        if (newPos != currentPos) {
                            ranges.push_back(std::make_pair(start, i));
                            start = i;
                            currentPos = newPos;
                        }
                    }
                }
                ranges.push_back(std::make_pair(start, end));
                break;
            }
            case Position::ALL:
                ALOGE("Not supported: field matcher with ALL position.");
                break;
            case Position::POSITION_UNKNOWN:
                break;
        }
    } else {
        // No position
        ranges.push_back(std::make_pair(start, end));
    }

    return ranges;
}

static bool matchesSimple(const sp<UidMap>& uidMap, const FieldValueMatcher& matcher,
                          const vector<FieldValue>& values, int start, int end, int depth) {
    if (depth > 2) {
        ALOGE("Depth > 3 not supported");
        return false;
    }

    if (start >= end) {
        return false;
    }

    const vector<pair<int, int>> ranges = computeRanges(matcher, values, start, end, depth);

    if (ranges.empty()) {
        // No such field found.
        return false;
    }

    // ranges should have exactly one start/end pair at this point unless position is ANY and
    // value_matcher is matches_tuple.
    std::tie(start, end) = ranges[0];

    switch (matcher.value_matcher_case()) {
        case FieldValueMatcher::kMatchesTuple: {
            ++depth;
            // If any range matches all matchers, good.
            for (const auto& [rangeStart, rangeEnd] : ranges) {
                bool matched = true;
                for (const auto& subMatcher : matcher.matches_tuple().field_value_matcher()) {
                    if (!matchesSimple(uidMap, subMatcher, values, rangeStart, rangeEnd, depth)) {
                        matched = false;
                        break;
                    }
                }
                if (matched) return true;
            }
            return false;
        }
        // Finally, we get to the point of real value matching.
        // If the field matcher ends with ANY, then we have [start, end) range > 1.
        // In the following, we should return true, when ANY of the values matches.
        case FieldValueMatcher::ValueMatcherCase::kEqBool: {
            for (int i = start; i < end; i++) {
                if ((values[i].mValue.getType() == INT &&
                     (values[i].mValue.int_value != 0) == matcher.eq_bool()) ||
                    (values[i].mValue.getType() == LONG &&
                     (values[i].mValue.long_value != 0) == matcher.eq_bool())) {
                    return true;
                }
            }
            return false;
        }
        case FieldValueMatcher::ValueMatcherCase::kEqString: {
            for (int i = start; i < end; i++) {
                if (tryMatchString(uidMap, values[i], matcher.eq_string())) {
                    return true;
                }
            }
            return false;
        }
        case FieldValueMatcher::ValueMatcherCase::kNeqAnyString: {
            const auto& str_list = matcher.neq_any_string();
            for (int i = start; i < end; i++) {
                bool notEqAll = true;
                for (const auto& str : str_list.str_value()) {
                    if (tryMatchString(uidMap, values[i], str)) {
                        notEqAll = false;
                        break;
                    }
                }
                if (notEqAll) {
                    return true;
                }
            }
            return false;
        }
        case FieldValueMatcher::ValueMatcherCase::kEqAnyString: {
            const auto& str_list = matcher.eq_any_string();
            for (int i = start; i < end; i++) {
                for (const auto& str : str_list.str_value()) {
                    if (tryMatchString(uidMap, values[i], str)) {
                        return true;
                    }
                }
            }
            return false;
        }
        case FieldValueMatcher::ValueMatcherCase::kEqWildcardString: {
            for (int i = start; i < end; i++) {
                if (tryMatchWildcardString(uidMap, values[i], matcher.eq_wildcard_string())) {
                    return true;
                }
            }
            return false;
        }
        case FieldValueMatcher::ValueMatcherCase::kEqAnyWildcardString: {
            const auto& str_list = matcher.eq_any_wildcard_string();
            for (int i = start; i < end; i++) {
                for (const auto& str : str_list.str_value()) {
                    if (tryMatchWildcardString(uidMap, values[i], str)) {
                        return true;
                    }
                }
            }
            return false;
        }
        case FieldValueMatcher::ValueMatcherCase::kNeqAnyWildcardString: {
            const auto& str_list = matcher.neq_any_wildcard_string();
            for (int i = start; i < end; i++) {
                bool notEqAll = true;
                for (const auto& str : str_list.str_value()) {
                    if (tryMatchWildcardString(uidMap, values[i], str)) {
                        notEqAll = false;
                        break;
                    }
                }
                if (notEqAll) {
                    return true;
                }
            }
            return false;
        }
        case FieldValueMatcher::ValueMatcherCase::kEqInt: {
            for (int i = start; i < end; i++) {
                if (values[i].mValue.getType() == INT &&
                    (matcher.eq_int() == values[i].mValue.int_value)) {
                    return true;
                }
                // eq_int covers both int and long.
                if (values[i].mValue.getType() == LONG &&
                    (matcher.eq_int() == values[i].mValue.long_value)) {
                    return true;
                }
            }
            return false;
        }
        case FieldValueMatcher::ValueMatcherCase::kEqAnyInt: {
            const auto& int_list = matcher.eq_any_int();
            for (int i = start; i < end; i++) {
                for (const int int_value : int_list.int_value()) {
                    if (values[i].mValue.getType() == INT &&
                        (int_value == values[i].mValue.int_value)) {
                        return true;
                    }
                    // eq_any_int covers both int and long.
                    if (values[i].mValue.getType() == LONG &&
                        (int_value == values[i].mValue.long_value)) {
                        return true;
                    }
                }
            }
            return false;
        }
        case FieldValueMatcher::ValueMatcherCase::kNeqAnyInt: {
            const auto& int_list = matcher.neq_any_int();
            for (int i = start; i < end; i++) {
                bool notEqAll = true;
                for (const int int_value : int_list.int_value()) {
                    if (values[i].mValue.getType() == INT &&
                        (int_value == values[i].mValue.int_value)) {
                        notEqAll = false;
                        break;
                    }
                    // neq_any_int covers both int and long.
                    if (values[i].mValue.getType() == LONG &&
                        (int_value == values[i].mValue.long_value)) {
                        notEqAll = false;
                        break;
                    }
                }
                if (notEqAll) {
                    return true;
                }
            }
            return false;
        }
        case FieldValueMatcher::ValueMatcherCase::kLtInt: {
            for (int i = start; i < end; i++) {
                if (values[i].mValue.getType() == INT &&
                    (values[i].mValue.int_value < matcher.lt_int())) {
                    return true;
                }
                // lt_int covers both int and long.
                if (values[i].mValue.getType() == LONG &&
                    (values[i].mValue.long_value < matcher.lt_int())) {
                    return true;
                }
            }
            return false;
        }
        case FieldValueMatcher::ValueMatcherCase::kGtInt: {
            for (int i = start; i < end; i++) {
                if (values[i].mValue.getType() == INT &&
                    (values[i].mValue.int_value > matcher.gt_int())) {
                    return true;
                }
                // gt_int covers both int and long.
                if (values[i].mValue.getType() == LONG &&
                    (values[i].mValue.long_value > matcher.gt_int())) {
                    return true;
                }
            }
            return false;
        }
        case FieldValueMatcher::ValueMatcherCase::kLtFloat: {
            for (int i = start; i < end; i++) {
                if (values[i].mValue.getType() == FLOAT &&
                    (values[i].mValue.float_value < matcher.lt_float())) {
                    return true;
                }
            }
            return false;
        }
        case FieldValueMatcher::ValueMatcherCase::kGtFloat: {
            for (int i = start; i < end; i++) {
                if (values[i].mValue.getType() == FLOAT &&
                    (values[i].mValue.float_value > matcher.gt_float())) {
                    return true;
                }
            }
            return false;
        }
        case FieldValueMatcher::ValueMatcherCase::kLteInt: {
            for (int i = start; i < end; i++) {
                if (values[i].mValue.getType() == INT &&
                    (values[i].mValue.int_value <= matcher.lte_int())) {
                    return true;
                }
                // lte_int covers both int and long.
                if (values[i].mValue.getType() == LONG &&
                    (values[i].mValue.long_value <= matcher.lte_int())) {
                    return true;
                }
            }
            return false;
        }
        case FieldValueMatcher::ValueMatcherCase::kGteInt: {
            for (int i = start; i < end; i++) {
                if (values[i].mValue.getType() == INT &&
                    (values[i].mValue.int_value >= matcher.gte_int())) {
                    return true;
                }
                // gte_int covers both int and long.
                if (values[i].mValue.getType() == LONG &&
                    (values[i].mValue.long_value >= matcher.gte_int())) {
                    return true;
                }
            }
            return false;
        }
        default:
            return false;
    }
}

bool matchesSimple(const sp<UidMap>& uidMap, const SimpleAtomMatcher& simpleMatcher,
                   const LogEvent& event) {
    if (event.GetTagId() != simpleMatcher.atom_id()) {
        return false;
    }

    for (const auto& matcher : simpleMatcher.field_value_matcher()) {
        if (!matchesSimple(uidMap, matcher, event.getValues(), 0, event.getValues().size(), 0)) {
            return false;
        }
    }
    return true;
}

}  // namespace statsd
}  // namespace os
}  // namespace android
