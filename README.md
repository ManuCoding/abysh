# abysh - the abyssal shell
Abysh is a deep and cool shell with the goal of being better than fish (the friendly interactive shell).

## Testing
Please fuzz this shell as much as possible! I want to find bugs in the `pipes` branch. Clone like this:
```sh
git clone -b pipes https://github.com/ManuCoding/abysh
```

## Building
This is a single-file C project, so you can simply use your favorite C compiler to compile `main.c` :D
```sh
gcc -O2 -o abysh main.c
```
Or, if you're lazy, just run the `build.sh` script:
```sh
./build.sh
```

## Features
It has a few readline-like shortcuts, environment variables are parsed and evaluated properly and a few builtins.
