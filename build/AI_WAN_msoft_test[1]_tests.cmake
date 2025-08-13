add_test([=[AIWANMsoftTest.RoutingTest]=]  /Users/nikahd/projects/msft-htsim/build/AI_WAN_msoft_test [==[--gtest_filter=AIWANMsoftTest.RoutingTest]==] --gtest_also_run_disabled_tests)
set_tests_properties([=[AIWANMsoftTest.RoutingTest]=]  PROPERTIES WORKING_DIRECTORY /Users/nikahd/projects/msft-htsim/build SKIP_REGULAR_EXPRESSION [==[\[  SKIPPED \]]==] LABELS unit TIMEOUT 60)
set(  AI_WAN_msoft_test_TESTS AIWANMsoftTest.RoutingTest)
