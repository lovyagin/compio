#ifdef COMPIO_DEBUG_PRINT
#define DEBUG_PRINT(...) fprintf(stderr, __VA_ARGS__)
#else
#define DEBUG_PRINT(...)                                                                           \
    do {                                                                                           \
    } while (0)
#endif

#ifdef COMPIO_WARNING_PRINT
#define WARNING_PRINT(...) fprintf(stderr, __VA_ARGS__)
#else
#define WARNING_PRINT(...)                                                                         \
    do {                                                                                           \
    } while (0)
#endif

#define UNUSED(x) (void)(x)