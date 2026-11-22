# LovyanGFX 1.2.7 has C sources that typedef bool; GCC 15 defaults to C23, where bool is a keyword.
Import("env")
env.Append(CFLAGS=["-std=gnu17"])
