#!/bin/sh
case "$CNET_TEST_DOTNET_MODE" in
failed) echo 'Test Run Aborted.'; exit 7 ;;
empty) echo 'No test matches the given testcase filter.'; exit 0 ;;
passed) echo 'Passed! - Failed: 0, Passed: 4, Skipped: 0, Total: 4'; exit 0 ;;
esac
exit 2
