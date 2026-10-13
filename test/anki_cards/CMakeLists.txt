add_executable(AnkiCardsTest AnkiCardsTest.cpp ${REPO_ROOT}/src/anki/AnkiCards.cpp)
target_link_libraries(AnkiCardsTest PRIVATE crosspoint_test_common GTest::gtest_main)
target_include_directories(AnkiCardsTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/Serialization
  ../library_builder/stubs
)
gtest_discover_tests(AnkiCardsTest TEST_PREFIX "AnkiCardsTest.")
