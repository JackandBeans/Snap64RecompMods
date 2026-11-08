# Island Skies' own compiler flags, added to the Makefile's (Nov 8 2026).
# No jump tables: the port's runtime recompiler of mod code did not resolve one, and
# the game stopped with "Failed to find function at 0x8100EA78" when a switch of the
# Pokémon's lighting compiled to one (JackandBeans, Nov 8 2026). A switch becomes
# a chain of tests instead.
MOD_CFLAGS := -fno-jump-tables
