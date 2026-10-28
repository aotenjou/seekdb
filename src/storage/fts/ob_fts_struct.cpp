/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "data_plane/fts/ob_fts_struct.h"

#include "lib/charset/ob_charset.h"
#include "share/datum/ob_datum_funcs.h"
#include "storage/ob_storage_util.h"

namespace oceanbase
{
namespace storage
{

int ObFTWord::hash(uint64_t &hash_val) const
{
  int ret = OB_SUCCESS;
  static thread_local common::ObDatumBasicFuncs *cached_funcs = nullptr;
  static thread_local ObObjType cached_type = ObMaxType;
  static thread_local ObCollationType cached_collation = CS_TYPE_INVALID;
  const ObObjType type = meta_.get_type();
  const ObCollationType collation = meta_.get_collation_type();
  if (OB_ISNULL(cached_funcs) || cached_type != type || cached_collation != collation) {
    cached_funcs = ObDatumFuncs::get_basic_func(type, collation);
    cached_type = type;
    cached_collation = collation;
  }
  common::ObDatumBasicFuncs *funcs = cached_funcs;
  if (OB_ISNULL(funcs)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (funcs->default_hash_ == nullptr) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    ret = funcs->default_hash_(word_, 0, hash_val, nullptr);
  }
  return ret;
}
bool ObFTWord::operator==(const ObFTWord &other) const
{
  bool is_equal = false;
  int ret = OB_SUCCESS;
  int cmp_ret = 0;
  if (word_.len_ == other.word_.len_
      && (0 == word_.len_ || 0 == MEMCMP(word_.ptr_, other.word_.ptr_, word_.len_))) {
    return true;
  }
  static thread_local ObDatumCmpFuncType cached_func = nullptr;
  static thread_local ObObjType cached_type = ObMaxType;
  static thread_local ObCollationType cached_collation = CS_TYPE_INVALID;
  static thread_local ObObjType cached_other_type = ObMaxType;
  static thread_local ObCollationType cached_other_collation = CS_TYPE_INVALID;
  if (OB_ISNULL(cached_func)
      || cached_type != meta_.get_type()
      || cached_collation != meta_.get_collation_type()
      || cached_other_type != other.meta_.get_type()
      || cached_other_collation != other.meta_.get_collation_type()) {
    cached_func = get_datum_cmp_func(meta_, other.meta_);
    cached_type = meta_.get_type();
    cached_collation = meta_.get_collation_type();
    cached_other_type = other.meta_.get_type();
    cached_other_collation = other.meta_.get_collation_type();
  }
  ObDatumCmpFuncType func = cached_func;
  if (func == nullptr) {
    ob_abort();
  } else if (OB_FAIL(func(word_, other.word_, cmp_ret, nullptr))) {
    ob_abort();
  } else {
    is_equal = (cmp_ret == 0);
  }
  return is_equal;
}
} // namespace storage
} // namespace oceanbase
