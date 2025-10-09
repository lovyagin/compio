#ifdef DEBUG_PRINT_ENABLED
#define DEBUG_PRINT(...) fprintf( stderr, __VA_ARGS__ )
#else
#define DEBUG_PRINT(...) do{ } while ( 0 )
#endif

#ifdef WARNING_PRINT_ENABLED
#define WARNING_PRINT(...) fprintf( stderr, __VA_ARGS__ )
#else
#define WARNING_PRINT(...) do{ } while ( 0 )
#endif
