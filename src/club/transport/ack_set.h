// Copyright 2016 Peter Jankuliak
// 
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// 
//     http://www.apache.org/licenses/LICENSE-2.0
// 
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef CLUB_TRANSPORT_ACK_SET_H
#define CLUB_TRANSPORT_ACK_SET_H

#include "sequence_number.h"

namespace club { namespace transport {

class AckSet {
public:
  AckSet();

  bool try_add(SequenceNumber new_sn);

  struct iterator {
    iterator(const AckSet& acks, size_t pos)
      : acks(acks), pos(pos) {}

    SequenceNumber operator*() const {
      assert(!acks.is_empty);
      return acks.highest_sequence_number - pos;
    }

    iterator& operator++() {// prefix
      while (pos < 32 && (acks.predecessors & (1 << (++pos - 1))) == 0) {}
      return *this;
    }

    iterator operator++(int) {// postfix
      auto ret = *this;
      ++(*this);
      return ret;
    }

    bool operator==(const iterator& other) const {
      return &acks == &other.acks && pos == other.pos;
    }

    bool operator!=(const iterator& other) const {
      return !(*this == other);
    }

    const AckSet& acks;
    size_t pos;
  };

  iterator begin() const {
    if (is_empty) return end();
    return iterator(*this, 0);
  }

  iterator end() const { return iterator(*this, 32); }

private:
  friend std::ostream& operator<<(std::ostream&, const AckSet&);

  SequenceNumber highest_sequence_number;
  SequenceNumber lowest_sequence_number;

  uint32_t predecessors : 31
         , is_empty     : 1;
};

//------------------------------------------------------------------------------
// Implementation
//------------------------------------------------------------------------------
inline AckSet::AckSet()
  : predecessors(0)
  , is_empty(true)
{}

inline bool AckSet::try_add(SequenceNumber new_sn) {
  if (is_empty) {
    highest_sequence_number = new_sn;
    lowest_sequence_number = new_sn;
    is_empty = false;
    predecessors = 0;
    return true;
  }
  else {
    auto hsn = highest_sequence_number;

    if (new_sn == hsn) {
      return true;
    }

    if (new_sn < hsn) {
      if (new_sn < hsn - 31) {
        return true;
      }
      
      predecessors |= 1 << (hsn - new_sn - 1);
      return true;
    }
    else {
      if (new_sn > hsn + 31) {
        return false;
      }

      auto shift = new_sn - hsn;

      for (decltype(shift) i = 0; i < shift; ++i) {
        if (!((hsn < lowest_sequence_number + 31 - i) || (predecessors & (1 << (30-i))))) {
          return false;
        }
      }

      auto was_empty = is_empty;
      is_empty = 0;
      predecessors <<= shift;
      predecessors |= 1 << (shift - 1);
      is_empty = was_empty;
      highest_sequence_number = new_sn;
      return true;
    }
  }
}

inline
std::ostream& operator<<(std::ostream& os, const AckSet& acks) {
  os << "(AckSet ";

  if (!acks.is_empty) {
    os << acks.highest_sequence_number << " ";
  }
  else {
    os << "none ";
  }

  os << (acks.is_empty ? "empty " : "not-empty ");

  for (int i = 0; i < 31; ++i) {
    os << ((acks.predecessors & (1 << i)) ? "1" : "0");
  }

  os << " {";

  for (auto i = acks.begin(); i != acks.end(); ++i) {
    os << *i << " ";
  }

  return os << "})";
}

}} // club::transport namespace

#endif // ifndef CLUB_TRANSPORT_ACK_SET_H
