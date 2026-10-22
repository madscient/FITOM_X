// tests/test_test_names.cpp
// テストケース名の制約を検査するテスト (Catch2 v3)

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_test_case_info.hpp>
#include <catch2/interfaces/catch_interfaces_registry_hub.hpp>
#include <catch2/interfaces/catch_interfaces_testcase.hpp>

#include <string>
#include <vector>

// テストケース名はASCIIに限る。Windowsでは、catch_discover_testsがテスト
// 一覧をコンソールのコードページ(CP932等)で読み、さらにテスト本体も
// 引数をそのコードページで受け取る。非ASCIIの名前はctest経由でだけ
// 「該当テスト無し」となって失敗し、exeを直接実行すると通ってしまう。
// 日本語の説明はSECTION名やコメントに書く(フィルタに使われないため無害)。
TEST_CASE("Test case names are ASCII only", "[meta]")
{
    std::vector<std::string> offenders;
    for (const auto* info :
         Catch::getRegistryHub().getTestCaseRegistry().getAllInfos()) {
        for (unsigned char c : info->name) {
            if (c >= 0x80) {
                offenders.push_back(info->name);
                break;
            }
        }
    }
    INFO("non-ASCII test case names: " << offenders.size());
    for (const auto& name : offenders)
        UNSCOPED_INFO(name);
    CHECK(offenders.empty());
}
