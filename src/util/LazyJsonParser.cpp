#include "LazyJsonParser.h"

#include <absl/strings/ascii.h>
#include <absl/strings/str_cat.h>
#include <absl/strings/str_join.h>

namespace ad_utility {

LazyJsonParser::LazyJsonParser(std::vector<std::string> arrayPath)
    : arrayPath_(arrayPath),
      prefixInArray_(absl::StrCat(
          absl::StrJoin(arrayPath.begin(), arrayPath.end(), "",
                        [](std::string* out, const std::string& s) {
                          absl::StrAppend(out, "{\"", s, "\": ");
                        }),
          "[")),
      suffixInArray_(absl::StrCat("]", std::string(arrayPath.size(), '}'))) {}

std::string LazyJsonParser::parseChunk(std::string inStr) {
  size_t idx = input_.size();
  absl::StrAppend(&input_, inStr);
  int materializeEnd = -1;

  if (inString_) {
    parseString(idx);
    ++idx;
  }

  if (inArrayPath_) {
    materializeEnd = parseArrayPath(idx);
    ++idx;
  }

  for (; idx < input_.size(); ++idx) {
    switch (input_[idx]) {
      case '{':
        if (strStart_ != -1 && strEnd_ != -1) {
          curPath_.emplace_back(input_.substr(strStart_, strEnd_ - strStart_));
        }
        break;
      case '[':
        if (openBrackets_ == 0 && strStart_ != -1 && strEnd_ != -1) {
          curPath_.emplace_back(input_.substr(strStart_, strEnd_ - strStart_));
        }
        ++openBrackets_;
        if (isInArrayPath()) {
          materializeEnd = parseArrayPath(idx);
        }
        break;
      case ']':
        --openBrackets_;
        if (openBrackets_ == 0 && !curPath_.empty()) {
          curPath_.pop_back();
          inArrayPath_ = isInArrayPath();
        }
        break;
      case '}':
        if (curPath_.empty()) {
          materializeEnd = idx + 1;
        } else {
          curPath_.pop_back();
        }
        break;
      case '"':
        parseString(idx);
        break;
    }
  }

  // Construct result.
  std::string res;
  if (materializeEnd == -1) {
    return res;
  }

  if (yieldCount_ > 0) {
    res = prefixInArray_;
  }
  ++yieldCount_;

  absl::StrAppend(&res, input_.substr(0, materializeEnd));
  input_ = materializeEnd < (int)input_.size()
               ? input_.substr(materializeEnd + 1)
               : "";

  if (inArrayPath_) {
    absl::StrAppend(&res, suffixInArray_);
  }

  return res;
}

int LazyJsonParser::parseArrayPath(size_t& idx) {
  int lastObjectEnd = -1;
  for (; idx < input_.size(); ++idx) {
    switch (input_[idx]) {
      case '{':
        ++openBracesInArrayPath_;
        break;
      case '[':
        if (inArrayPath_) {
          ++openBracesInArrayPath_;
        } else {
          inArrayPath_ = true;
        }
        break;
      case '}':
        --openBracesInArrayPath_;
        break;
      case ']':
        if (openBracesInArrayPath_ == 0) {
          inArrayPath_ = false;
          if (!curPath_.empty()) {
            curPath_.pop_back();
          }
          return lastObjectEnd;
        }
        --openBracesInArrayPath_;
        break;
      case ',':
        if (openBracesInArrayPath_ == 0) {
          lastObjectEnd = idx;
        }
        break;
      case '"':
        parseString(idx);
        break;
    }
  }
  return lastObjectEnd;
}

void LazyJsonParser::parseString(size_t& idx) {
  for (; idx < input_.size(); ++idx) {
    if (isEscaped_) {
      isEscaped_ = false;
      continue;
    }
    switch (input_[idx]) {
      case '"':
        inString_ = !inString_;
        if (inString_) {
          strStart_ = idx + 1;
        } else {
          strEnd_ = idx;
          return;
        }
        break;
      case '\\':
        isEscaped_ = true;
        break;
    }
  }
}

}  // namespace ad_utility
