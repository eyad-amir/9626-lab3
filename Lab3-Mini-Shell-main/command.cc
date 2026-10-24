#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <glob.h>
#include <wordexp.h>
#include <vector>
#include <string>
#include <cctype>
#include <iostream>
#include <sstream>
#include "command.h"
#include "tokenizer.h"
#include <errno.h>
static int logFd = -1;

static void logChildTermination() {
    const char message[] = "Child terminated\n";
    int written = 0;
    int length = sizeof(message) - 1;

    while (written < length) {
        ssize_t result = write(logFd, message + written, length - written);
        if (result > 0) { written += result; }
        else if (result < 0 && errno == EINTR) { continue; }
        else { break; }}}

static void handleChildren(int) {
    int savedErrno = errno;
    int status;

    while (true) {
        pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid > 0) { logChildTermination(); }
        else if (pid < 0 && errno == EINTR) { continue; }
        else { break; }}
    errno = savedErrno;}

void parse(std::vector<Token> &tokens) {
    std::vector<std::string> patterns(tokens.size());
    std::vector<bool> expandWildcard(tokens.size(), false);
    // Remove grouping quotes, keeping quoted wildcards literal in glob patterns.
    for (int i = 0; i < (int)tokens.size(); i++) {
        if (tokens[i].type != TOKEN_COMMAND && tokens[i].type != TOKEN_ARGUMENT) { continue; }
        std::string word, pattern;
        char quote = 0;
        const std::string &raw = tokens[i].value;
        for (int j = 0; j < (int)raw.size(); j++) {
            char character = raw[j];
            bool escaped = false;
            if (character == '\\' && quote != '\'') {
                if (j + 1 >= (int)raw.size()) {
                    fprintf(stderr, "Trailing backslash\n");
                    return;}
                char next = raw[j + 1];
                if (quote == '"' && next != '"' && next != '\\' && next != '$' && next != '`') {
                    word += character;
                    pattern += "\\\\";
                    continue;}
                character = raw[++j];
                escaped = true;}
            else if (quote && character == quote) { quote = 0; continue; }
            else if (!quote && (character == '\'' || character == '"')) {
                quote = character;
                continue;}
            word += character;
            if ((quote || escaped) && strchr("*?[]\\", character)) { pattern += '\\'; }
            pattern += character;
            if (!quote && !escaped && (character == '*' || character == '?')) { expandWildcard[i] = true; }}
        if (quote) { fprintf(stderr, "Unmatched quote\n"); return; }
        tokens[i].value = word;
        patterns[i] = pattern;}

    Command currentCommand;
    SimpleCommand *currentSimpleCommand = new SimpleCommand();

    currentCommand.insertSimpleCommand(currentSimpleCommand);
    bool valid = true;

    for (int i = 0; i < (int)tokens.size(); i++) {
        if (tokens[i].type == TOKEN_EOF) { break; }
        else if (tokens[i].type == TOKEN_COMMAND || tokens[i].type == TOKEN_ARGUMENT) {
            if (expandWildcard[i]) {
                glob_t matches = {};
                int result = glob(patterns[i].c_str(), 0, NULL, &matches);
                if (result == 0) {
                    for (size_t j = 0; j < matches.gl_pathc; j++) {
                        currentSimpleCommand->insertArgument(strdup(matches.gl_pathv[j]));}}
                else if (result == GLOB_NOMATCH) {
                    currentSimpleCommand->insertArgument(strdup(tokens[i].value.c_str()));}
                else { fprintf(stderr, "Wildcard expansion failed\n"); valid = false; }
                globfree(&matches);
                if (!valid) { break; }}
            else { currentSimpleCommand->insertArgument(strdup(tokens[i].value.c_str())); }}
        else if (tokens[i].type == TOKEN_PIPE) {
            if (currentSimpleCommand->_numberOfArguments == 0) {
                fprintf(stderr, "Missing command before pipe\n");
                valid = false;
                break;}
            currentSimpleCommand = new SimpleCommand();
            currentCommand.insertSimpleCommand(currentSimpleCommand);}
        else if (tokens[i].type == TOKEN_BACKGROUND) {
            if (currentSimpleCommand->_numberOfArguments == 0 || i + 1 >= (int)tokens.size() || tokens[i + 1].type != TOKEN_EOF) {
                fprintf(stderr, "& must be at the end of a command\n");
                valid = false;
                break;}
            currentCommand._background = 1;}
        else if (tokens[i].type == TOKEN_REDIRECT || tokens[i].type == TOKEN_APPEND || tokens[i].type == TOKEN_INPUT || tokens[i].type == TOKEN_ERROR || tokens[i].type == TOKEN_REDIRECT_AND_ERROR) {
            TokenType redirectType = tokens[i].type;
            if (i + 1 >= (int)tokens.size() || (tokens[i + 1].type != TOKEN_COMMAND && tokens[i + 1].type != TOKEN_ARGUMENT)) {
                fprintf(stderr, "Missing filename after redirection\n");
                valid = false;
                break;}
            bool duplicate = false;
            if (redirectType == TOKEN_INPUT) { duplicate = currentCommand._inputFile != NULL; }
            else if (redirectType == TOKEN_ERROR) { duplicate = currentCommand._errFile != NULL; }
            else if (redirectType == TOKEN_REDIRECT_AND_ERROR) {
                duplicate = currentCommand._outFile != NULL ||
                            currentCommand._errFile != NULL;}
            else { duplicate = currentCommand._outFile != NULL; }
            if (duplicate) { fprintf(stderr, "Duplicate redirection\n"); valid = false; break; }

            // Move to the filename so it is not added as an argument.
            i++;
            if (expandWildcard[i]) {
                glob_t matches = {};
                int result = glob(patterns[i].c_str(), 0, NULL, &matches);
                if (result == 0 && matches.gl_pathc == 1) { tokens[i].value = matches.gl_pathv[0]; }
                else if (result != GLOB_NOMATCH) {
                    fprintf(stderr, "Redirection must match exactly one file\n");
                    valid = false;}
                globfree(&matches);
                if (!valid) { break; }}
            if (tokens[i].value.empty()) {
                fprintf(stderr, "Empty redirection filename\n");
                valid = false;
                break;}
            const char *filename = tokens[i].value.c_str();
            if (redirectType == TOKEN_INPUT) { currentCommand._inputFile = strdup(filename); }
            else if (redirectType == TOKEN_ERROR) { currentCommand._errFile = strdup(filename); }
            else if (redirectType == TOKEN_REDIRECT_AND_ERROR) {
                // Each field owns a separate copy of the filename.
                currentCommand._outFile = strdup(filename);
                currentCommand._errFile = strdup(filename);
                currentCommand._out_error = 1;
                currentCommand._append = 1;}
            else {
                currentCommand._outFile = strdup(filename);
                if (redirectType == TOKEN_APPEND) { currentCommand._append = 1; }
                else { currentCommand._append = 0; }}}
        else { fprintf(stderr, "Invalid token\n"); valid = false; break; }}

    // Reject an empty final command, but keep blank input quiet.
    if (valid && currentSimpleCommand->_numberOfArguments == 0) {
        if (currentCommand._numberOfSimpleCommands > 1 || currentCommand._inputFile != NULL || currentCommand._outFile != NULL || currentCommand._errFile != NULL) {
            fprintf(stderr, "Missing command\n");}
        valid = false;}

    if (valid) { currentCommand.execute(); }
    else { currentCommand.clear(); }

    free(currentCommand._simpleCommands);}


SimpleCommand::SimpleCommand() {
    _numberOfAvailableArguments = 5;
    _numberOfArguments = 0;
    _arguments = (char **)malloc(_numberOfAvailableArguments * sizeof(char *));
    _arguments[0] = NULL;}

void SimpleCommand::insertArgument(char *argument) {
    if (_numberOfAvailableArguments == _numberOfArguments + 1) {
        _numberOfAvailableArguments *= 2;
        _arguments = (char **)realloc(_arguments, _numberOfAvailableArguments * sizeof(char *));}

    _arguments[_numberOfArguments] = argument;
    _arguments[_numberOfArguments + 1] = NULL;

    _numberOfArguments++;}

Command::Command() {
    _numberOfAvailableSimpleCommands = 1;
    _simpleCommands = (SimpleCommand **)
        malloc(_numberOfAvailableSimpleCommands * sizeof(SimpleCommand *));

    _numberOfSimpleCommands = 0;
    _outFile = 0;
    _inputFile = 0;
    _errFile = 0;
    _background = 0;
    _append = 0;
    _out_error = 0;}

void Command::insertSimpleCommand(SimpleCommand *simpleCommand) {
    if (_numberOfAvailableSimpleCommands == _numberOfSimpleCommands) {
        _numberOfAvailableSimpleCommands *= 2;
        _simpleCommands = (SimpleCommand **)realloc(_simpleCommands, _numberOfAvailableSimpleCommands * sizeof(SimpleCommand *));}

    _simpleCommands[_numberOfSimpleCommands] = simpleCommand;
    _numberOfSimpleCommands++;}

void Command::clear() {
    for (int i = 0; i < _numberOfSimpleCommands; i++) {
        for (int j = 0; j < _simpleCommands[i]->_numberOfArguments; j++) {
            free(_simpleCommands[i]->_arguments[j]);}
        free(_simpleCommands[i]->_arguments);
        delete _simpleCommands[i];}

    if (_outFile) { free(_outFile); }

    if (_inputFile) { free(_inputFile); }

    if (_errFile) { free(_errFile); }

    _numberOfSimpleCommands = 0;
    _outFile = 0;
    _inputFile = 0;
    _errFile = 0;
    _background = 0;
    _append = 0;
    _out_error = 0;}

void Command::print() {
    printf("\n\n");
    printf("              COMMAND TABLE                \n");
    printf("\n");
    printf("  #   Simple Commands\n");
    printf("  --- ----------------------------------------------------------\n");

    for (int i = 0; i < _numberOfSimpleCommands; i++) {
        printf("  %-3d ", i);
        for (int j = 0; j < _simpleCommands[i]->_numberOfArguments; j++) {
            printf("\"%s\" \t", _simpleCommands[i]->_arguments[j]);}
        printf("\n");}

    printf("\n\n");
    printf("  Output       Input        Error        Err&Out       Background\n");
    printf("  ------------ ------------ ------------ ------------ ------------\n");
    printf("  %-12s %-12s %-12s %-12s %-12s\n", _outFile ? _outFile : "default", _inputFile ? _inputFile : "default", _errFile ? _errFile : "default", _out_error == 1 ? _errFile : "default" ,_background ? "YES" : "NO");
    printf("\n\n");}
void Command::execute() {
    if (_numberOfSimpleCommands == 0) { clear(); return; }

    if (_numberOfSimpleCommands == 1 && strcmp(_simpleCommands[0]->_arguments[0], "exit") == 0) {
        printf("Good bye!!\n");
        clear();
        exit(0);}

    if (_numberOfSimpleCommands == 1 && strcmp(_simpleCommands[0]->_arguments[0], "cd") == 0) {
        SimpleCommand *command = _simpleCommands[0];
        if (command->_numberOfArguments > 2) {
            fprintf(stderr, "cd: too many arguments\n");
            clear();
            return;}
        const char *directory;
        if (command->_numberOfArguments == 1) { directory = getenv("HOME"); }
        else { directory = command->_arguments[1]; }
        if (directory == NULL) { fprintf(stderr, "cd: HOME is not set\n"); }
        else if (chdir(directory) < 0) { perror("cd"); }
        clear();
        return;}
    int inputFd = STDIN_FILENO;
    int outputFd = STDOUT_FILENO;
    int errorFd = STDERR_FILENO;
    bool valid = true;
    if (_inputFile != NULL) {
        inputFd = open(_inputFile, O_RDONLY);
        if (inputFd < 0) { perror(_inputFile); valid = false; }}
    if (valid && _outFile != NULL) {
        int flags = O_WRONLY | O_CREAT;
        if (_append) { flags |= O_APPEND; }
        else { flags |= O_TRUNC; }
        outputFd = open(_outFile, flags, 0666);
        if (outputFd < 0) { perror(_outFile); valid = false; }}
    if (valid && _errFile != NULL) {
        if (_out_error) { errorFd = outputFd; }
        else {
            errorFd = open(_errFile, O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (errorFd < 0) { perror(_errFile); valid = false; }}}
    if (!valid) {
        if (inputFd > 2) { close(inputFd); }
        if (outputFd > 2 && outputFd != inputFd) { close(outputFd); }
        if (errorFd > 2 && errorFd != inputFd && errorFd != outputFd) { close(errorFd); }
        clear();
        return;}

    // Delay child collection until pipeline creation and failure cleanup finish.
    sigset_t childSignals, oldMask;
    sigemptyset(&childSignals);
    sigaddset(&childSignals, SIGCHLD);
    if (sigprocmask(SIG_BLOCK, &childSignals, &oldMask) < 0) {
        perror("sigprocmask");
        if (inputFd > 2) { close(inputFd); }
        if (outputFd > 2 && outputFd != inputFd) { close(outputFd); }
        if (errorFd > 2 && errorFd != inputFd && errorFd != outputFd) { close(errorFd); }
        clear();
        return;}
    std::vector<pid_t> children;
    int previousFd = inputFd;
    fflush(NULL);
    for (int i = 0; i < _numberOfSimpleCommands; i++) {
        int pipeFd[2];
        bool lastCommand = (i == _numberOfSimpleCommands - 1);
        if (!lastCommand) {
            if (pipe(pipeFd) < 0) { perror("pipe"); valid = false; break; }}
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            if (!lastCommand) { close(pipeFd[0]); close(pipeFd[1]); }
            valid = false;
            break;}
        if (pid == 0) {
            struct sigaction action = {};
            action.sa_handler = SIG_DFL;
            sigemptyset(&action.sa_mask);
            if (sigaction(SIGINT, &action, NULL) < 0 || sigaction(SIGCHLD, &action, NULL) < 0) {
                perror("sigaction");
                _exit(1);}
            if (sigprocmask(SIG_SETMASK, &oldMask, NULL) < 0) {
                perror("sigprocmask");
                _exit(1);}
            if (previousFd != STDIN_FILENO && dup2(previousFd, STDIN_FILENO) < 0) {
                perror("redirect input");
                _exit(1);}
            int childOutputFd;
            if (lastCommand) { childOutputFd = outputFd; }
            else { childOutputFd = pipeFd[1]; }
            if (childOutputFd != STDOUT_FILENO && dup2(childOutputFd, STDOUT_FILENO) < 0) {
                perror("redirect output");
                _exit(1);}
            if (errorFd != STDERR_FILENO && dup2(errorFd, STDERR_FILENO) < 0) {
                perror("redirect error");
                _exit(1);}
            if (inputFd > 2) { close(inputFd); }
            if (outputFd > 2 && outputFd != inputFd) { close(outputFd); }
            if (errorFd > 2 && errorFd != inputFd && errorFd != outputFd) { close(errorFd); }
            if (previousFd > 2 && previousFd != inputFd) { close(previousFd); }
            if (!lastCommand) { close(pipeFd[0]); close(pipeFd[1]); }
            execvp(_simpleCommands[i]->_arguments[0], _simpleCommands[i]->_arguments);
            perror(_simpleCommands[i]->_arguments[0]);
            _exit(127);}
        children.push_back(pid);
        if (previousFd > 2 && previousFd != inputFd) { close(previousFd); }
        if (!lastCommand) { close(pipeFd[1]); previousFd = pipeFd[0]; }
        else { previousFd = -1; }}
    if (previousFd > 2 && previousFd != inputFd) { close(previousFd); }
    if (inputFd > 2) { close(inputFd); }
    if (outputFd > 2 && outputFd != inputFd) { close(outputFd); }
    if (errorFd > 2 && errorFd != inputFd && errorFd != outputFd) { close(errorFd); }
    if (!valid) {
        for (int i = 0; i < (int)children.size(); i++) {
            kill(children[i], SIGKILL);}}

    // Allow the SIGCHLD handler to collect finished children again.
    if (sigprocmask(SIG_SETMASK, &oldMask, NULL) < 0) {
        perror("sigprocmask");
        exit(1);}
    if (!_background || !valid) {
        for (int i = 0; i < (int)children.size(); i++) {
            int status;
            pid_t result;
            do {
                result = waitpid(children[i], &status, 0);}
            while (result < 0 && errno == EINTR);
            if (result > 0) { logChildTermination(); }
            else if (result < 0 && errno != ECHILD) { perror("waitpid"); }}}
    else if (!children.empty()) {
        printf("[background] %ld\n", (long)children.back());}
    clear();}
void Command::prompt() {
    std::string input;
    while (true) {
        printf("myshell>");
        fflush(stdout);
        if (!std::getline(std::cin, input)) { break; }

        // Scan here because tokenizer.cc must stay unchanged.
        std::vector<Token> tokens;
        bool valid = true;
        int i = 0;
        while (i < (int)input.size()) {
            if (std::isspace((unsigned char)input[i])) { i++; continue; }
            Token token;
            int start = i;

            // Recognize longer operators before their shorter prefixes.
            if (input.compare(i, 3, ">>&") == 0) {
                token.type = TOKEN_REDIRECT_AND_ERROR;
                i += 3;}
            else if (input.compare(i, 2, ">>") == 0) {
                token.type = TOKEN_APPEND;
                i += 2;}
            else if (input.compare(i, 2, "2>") == 0) {
                token.type = TOKEN_ERROR;
                i += 2;}
            else if (input[i] == '>') { token.type = TOKEN_REDIRECT; i++; }
            else if (input[i] == '<') { token.type = TOKEN_INPUT; i++; }
            else if (input[i] == '|') { token.type = TOKEN_PIPE; i++; }
            else if (input[i] == '&') { token.type = TOKEN_BACKGROUND; i++; }
            else {
                token.type = TOKEN_ARGUMENT;
                char quote = 0;
                while (i < (int)input.size()) {
                    char character = input[i];
                    if (character == '\\' && quote != '\'') {
                        if (i + 1 >= (int)input.size()) {
                            fprintf(stderr, "Trailing backslash\n");
                            valid = false;
                            break;}
                        i += 2;
                        continue;}
                    if (quote) {
                        if (character == quote) { quote = 0; }}
                    else if (character == '\'' || character == '"') { quote = character; }
                    else if (std::isspace((unsigned char)character) || character == '|' || character == '<' || character == '>' || character == '&') { break; }
                    i++;}
                if (!valid) { break; }
                if (quote) { fprintf(stderr, "Unmatched quote\n"); valid = false; break; }}
            token.value = input.substr(start, i - start);
            tokens.push_back(token);}
        if (valid) {
            tokens.push_back({TOKEN_EOF, ""});
            parse(tokens);}}}

Command Command::_currentCommand;
SimpleCommand *Command::_currentSimpleCommand;

int main() {
    logFd = open("shell.log", O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
    if (logFd < 0) { perror("shell.log"); return 1; }
    struct sigaction action = {};
    action.sa_handler = SIG_IGN;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) < 0) {
        perror("sigaction");
        close(logFd);
        return 1;}
    action.sa_handler = handleChildren;
    action.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &action, NULL) < 0) {
        perror("sigaction");
        close(logFd);
        return 1;}
    Command::_currentCommand.prompt();
    return 0;}
