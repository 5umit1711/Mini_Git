# CMake generated Testfile for 
# Source directory: D:/CODE/Mini_Git
# Build directory: D:/CODE/Mini_Git/build
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test("sha1" "D:/CODE/Mini_Git/build/test_sha1.exe")
set_tests_properties("sha1" PROPERTIES  _BACKTRACE_TRIPLES "D:/CODE/Mini_Git/CMakeLists.txt;42;add_test;D:/CODE/Mini_Git/CMakeLists.txt;0;")
add_test("workflow" "C:/Users/ASUS/AppData/Local/Programs/Python/Python314/python.exe" "D:/CODE/Mini_Git/tests/workflow.py" "D:/CODE/Mini_Git/build/mygit.exe")
set_tests_properties("workflow" PROPERTIES  _BACKTRACE_TRIPLES "D:/CODE/Mini_Git/CMakeLists.txt;46;add_test;D:/CODE/Mini_Git/CMakeLists.txt;0;")
