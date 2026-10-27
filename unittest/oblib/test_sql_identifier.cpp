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

#include <cstdlib>
#include <string>
#include <vector>
#include <gtest/gtest.h>
#include "lib/string/ob_sql_string.h"

using namespace oceanbase::common;

namespace
{
class IdentifierAllocator : public ObIAllocator
{
public:
  ~IdentifierAllocator() override
  {
    for (void *ptr : allocations_) {
      std::free(ptr);
    }
  }
  void *alloc(const int64_t size) override
  {
    ++calls_;
    void *ptr = fail_ ? nullptr : std::malloc(size);
    if (nullptr != ptr) {
      allocations_.push_back(ptr);
    }
    return ptr;
  }
  void *alloc(const int64_t size, const ObMemAttr &) override { return alloc(size); }
  void free(void *) override {}
  bool fail_ = false;
  int calls_ = 0;
private:
  std::vector<void *> allocations_;
};

TEST(SqlIdentifier, UnquotedInputAliasesSource)
{
  IdentifierAllocator allocator;
  const ObString source = ObString::make_string("a normal column");
  ObString escaped;
  ASSERT_EQ(OB_SUCCESS, ObSqlString::escape_identifier(allocator, source, escaped));
  EXPECT_EQ(source.ptr(), escaped.ptr());
  EXPECT_EQ(source.length(), escaped.length());
  EXPECT_EQ(0, allocator.calls_);
}

TEST(SqlIdentifier, EmptyInputClearsPreviousOutput)
{
  IdentifierAllocator allocator;
  ObString escaped = ObString::make_string("previous");
  ASSERT_EQ(OB_SUCCESS, ObSqlString::escape_identifier(allocator, ObString(), escaped));
  EXPECT_EQ(nullptr, escaped.ptr());
  EXPECT_EQ(0, escaped.length());
  EXPECT_EQ(0, allocator.calls_);
}

TEST(SqlIdentifier, EscapesEveryDelimiter)
{
  IdentifierAllocator allocator;
  const char *sources[] = {"`", "a`b", "`first", "last`", "a``b", "`a``b`", "列`名"};
  const char *expected[] = {"``", "a``b", "``first", "last``", "a````b", "``a````b``", "列``名"};
  for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); ++i) {
    ObString escaped;
    const ObString source = ObString::make_string(sources[i]);
    ASSERT_EQ(OB_SUCCESS, ObSqlString::escape_identifier(allocator, source, escaped));
    EXPECT_EQ(std::string(expected[i]), std::string(escaped.ptr(), escaped.length()));
    EXPECT_NE(source.ptr(), escaped.ptr());
    EXPECT_EQ('\0', escaped.ptr()[escaped.length()]);
  }
}

TEST(SqlIdentifier, UsesExplicitLength)
{
  IdentifierAllocator allocator;
  char input[] = {'a', '`', 'b', 'x'};
  ObString escaped;
  ASSERT_EQ(OB_SUCCESS, ObSqlString::escape_identifier(allocator, ObString(3, input), escaped));
  EXPECT_EQ("a``b", std::string(escaped.ptr(), escaped.length()));
  EXPECT_EQ('x', input[3]);
}

TEST(SqlIdentifier, CanReplaceSourceView)
{
  IdentifierAllocator allocator;
  ObString value = ObString::make_string("a`b");
  ASSERT_EQ(OB_SUCCESS, ObSqlString::escape_identifier(allocator, value, value));
  EXPECT_EQ("a``b", std::string(value.ptr(), value.length()));
}

TEST(SqlIdentifier, ReportsAllocationFailureAndClearsOutput)
{
  IdentifierAllocator allocator;
  allocator.fail_ = true;
  ObString escaped = ObString::make_string("previous");
  EXPECT_EQ(OB_ALLOCATE_MEMORY_FAILED,
            ObSqlString::escape_identifier(allocator, ObString::make_string("a`b"), escaped));
  EXPECT_EQ(nullptr, escaped.ptr());
  EXPECT_EQ(0, escaped.length());
  EXPECT_EQ(1, allocator.calls_);
}
} // namespace

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
