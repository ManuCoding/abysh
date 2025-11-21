# abysh - the abyssal shell
Abysh is a deep and cool shell with the goal of being better than fish (the friendly interactive shell).

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
- Readline-like text movement commands
- Kill ring
- History
- Command piping
- The chdir (cd) command
- Environment variable assignment and expansion
- Temporary variable handling
- Uses common variables like `PATH`, `HOME` and `SHLVL`
- String unescaping
- Comments
- Expanding environment variables when mixed with text
- Saving history to a file
- Expanding ~ to the HOME environment variable

## Upcoming Features
- File stream redirections
- Capture child process signals
- Background jobs
- History fuzzy navigation (C-r/C-s)
- Evaluating script files (shebang)
- Acting normally over SSH
- Coloooooors and customization
- Handling the `.abyshrc` file
- Add the ability to enable/disable features
- Cool logo
