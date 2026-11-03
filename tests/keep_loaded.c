// Linked into the tests of sanitizer builds (tests/CMakeLists.txt). GPU drivers leak a few
// one-time allocations, which tools/lsan.supp suppresses by library name; but the Vulkan
// loader unloads the drivers before exit, and LeakSanitizer can't name the library of a
// leak whose library is gone ("<unknown module>"). Keeping every shared object loaded until
// exit keeps the names. dlclose is interposed from the executable, so the loader's calls
// land here.
int dlclose(void *handle);

int dlclose(void *handle) {
    (void)handle;
    return 0;
}
