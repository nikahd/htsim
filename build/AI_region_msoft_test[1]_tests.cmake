add_test([=[AIRegionMsoftTest.RoutingTest]=]  /Users/nikahd/projects/msft-htsim/build/AI_region_msoft_test [==[--gtest_filter=AIRegionMsoftTest.RoutingTest]==] --gtest_also_run_disabled_tests)
set_tests_properties([=[AIRegionMsoftTest.RoutingTest]=]  PROPERTIES WORKING_DIRECTORY /Users/nikahd/projects/msft-htsim/build SKIP_REGULAR_EXPRESSION [==[\[  SKIPPED \]]==] LABELS unit TIMEOUT 60)
set(  AI_region_msoft_test_TESTS AIRegionMsoftTest.RoutingTest)
