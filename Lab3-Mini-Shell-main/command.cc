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

void parse(std::vector<Token> &tokens) {
    Command currentCommand;
    SimpleCommand *currentSimpleCommand = new SimpleCommand();

    currentCommand.insertSimpleCommand(currentSimpleCommand);
    bool valid = true;

    for (int i = 0; i < (int)tokens.size(); i++) {
        if (tokens[i].type == TOKEN_EOF) { break; }
        else if (tokens[i].type == TOKEN_COMMAND ||
                 tokens[i].type == TOKEN_ARGUMENT) {
            char *argument = strdup(tokens[i].value.c_str());
            currentSimpleCommand->insertArgument(argument);
        }
        else if (tokens[i].type == TOKEN_PIPE) {
            if (currentSimpleCommand->_numberOfArguments == 0) {
                fprintf(stderr, "Missing command before pipe\n");
                valid = false;
                break;
            }

            currentSimpleCommand = new SimpleCommand();
            currentCommand.insertSimpleCommand(currentSimpleCommand);
        }
        else if (tokens[i].type == TOKEN_BACKGROUND) {
            if (currentSimpleCommand->_numberOfArguments == 0 ||
                i + 1 >= (int)tokens.size() ||
                tokens[i + 1].type != TOKEN_EOF) {
                fprintf(stderr, "& must be at the end of a command\n");
                valid = false;
                break;
            }

            currentCommand._background = 1;
        }
        else if (tokens[i].type == TOKEN_REDIRECT ||
                 tokens[i].type == TOKEN_APPEND ||
                 tokens[i].type == TOKEN_INPUT ||
                 tokens[i].type == TOKEN_ERROR ||
                 tokens[i].type == TOKEN_REDIRECT_AND_ERROR) {
            TokenType redirectType = tokens[i].type;

            if (i + 1 >= (int)tokens.size() ||
                (tokens[i + 1].type != TOKEN_COMMAND &&
                 tokens[i + 1].type != TOKEN_ARGUMENT)) {
                fprintf(stderr, "Missing filename after redirection\n");
                valid = false;
                break;
            }

            bool duplicate = false;

            if (redirectType == TOKEN_INPUT) { duplicate = currentCommand._inputFile != NULL; }
            else if (redirectType == TOKEN_ERROR) { duplicate = currentCommand._errFile != NULL; }
            else if (redirectType == TOKEN_REDIRECT_AND_ERROR) {
                duplicate = currentCommand._outFile != NULL ||
                            currentCommand._errFile != NULL;
            }
            else { duplicate = currentCommand._outFile != NULL; }

            if (duplicate) { fprintf(stderr, "Duplicate redirection\n"); valid = false; break; }

            // Move to the filename so it is not added as an argument.
            i++;
            const char *filename = tokens[i].value.c_str();

            if (redirectType == TOKEN_INPUT) { currentCommand._inputFile = strdup(filename); }
            else if (redirectType == TOKEN_ERROR) { currentCommand._errFile = strdup(filename); }
            else if (redirectType == TOKEN_REDIRECT_AND_ERROR) {
                // Each field owns a separate copy of the filename.
                currentCommand._outFile = strdup(filename);
                currentCommand._errFile = strdup(filename);
                currentCommand._out_error = 1;
                currentCommand._append = 1;
            }
            else {
                currentCommand._outFile = strdup(filename);

                if (redirectType == TOKEN_APPEND) { currentCommand._append = 1; }
                else { currentCommand._append = 0; }
            }
        }
        else { fprintf(stderr, "Invalid token\n"); valid = false; break; }
    }

    // Reject an empty final command, but keep blank input quiet.
    if (valid && currentSimpleCommand->_numberOfArguments == 0) {
        if (currentCommand._numberOfSimpleCommands > 1 ||
            currentCommand._inputFile != NULL ||
            currentCommand._outFile != NULL ||
            currentCommand._errFile != NULL) {
            fprintf(stderr, "Missing command\n");
        }

        valid = false;
    }

    if (valid) { currentCommand.execute(); }
    else { currentCommand.clear(); }

    free(currentCommand._simpleCommands);
}

SimpleCommand::SimpleCommand() {
    _numberOfAvailableArguments = 5;
    _numberOfArguments = 0;
    _arguments = (char **)malloc(_numberOfAvailableArguments * sizeof(char *));
    _arguments[0] = NULL;
}

void SimpleCommand::insertArgument(char *argument) {
    if (_numberOfAvailableArguments == _numberOfArguments + 1) {
        _numberOfAvailableArguments *= 2;
        _arguments = (char **)realloc(_arguments,
                                      _numberOfAvailableArguments * sizeof(char *));
    }

    _arguments[_numberOfArguments] = argument;
    _arguments[_numberOfArguments + 1] = NULL;

    _numberOfArguments++;
}

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
    _out_error = 0;
}

void Command::insertSimpleCommand(SimpleCommand *simpleCommand) {
    if (_numberOfAvailableSimpleCommands == _numberOfSimpleCommands) {
        _numberOfAvailableSimpleCommands *= 2;
        _simpleCommands = (SimpleCommand **)realloc(_simpleCommands,
                                                    _numberOfAvailableSimpleCommands * sizeof(SimpleCommand *));
    }

    _simpleCommands[_numberOfSimpleCommands] = simpleCommand;
    _numberOfSimpleCommands++;
}

void Command::clear() {
    for (int i = 0; i < _numberOfSimpleCommands; i++) {
        for (int j = 0; j < _simpleCommands[i]->_numberOfArguments; j++) {
            free(_simpleCommands[i]->_arguments[j]);
        }

        free(_simpleCommands[i]->_arguments);
        delete _simpleCommands[i];
    }

    if (_outFile) { free(_outFile); }

    if (_inputFile) { free(_inputFile); }

    if (_errFile) { free(_errFile); }

    _numberOfSimpleCommands = 0;
    _outFile = 0;
    _inputFile = 0;
    _errFile = 0;
    _background = 0;
    _append = 0;
    _out_error = 0;
}

void Command::print() {
    printf("\n\n");
    printf("              COMMAND TABLE                \n");
    printf("\n");
    printf("  #   Simple Commands\n");
    printf("  --- ----------------------------------------------------------\n");

    for (int i = 0; i < _numberOfSimpleCommands; i++) {
        printf("  %-3d ", i);
        for (int j = 0; j < _simpleCommands[i]->_numberOfArguments; j++) {
            printf("\"%s\" \t", _simpleCommands[i]->_arguments[j]);
        }
        printf("\n");
    }

    printf("\n\n");
    printf("  Output       Input        Error        Err&Out       Background\n");
    printf("  ------------ ------------ ------------ ------------ ------------\n");
    printf("  %-12s %-12s %-12s %-12s %-12s\n", _outFile ? _outFile : "default",
           _inputFile ? _inputFile : "default", _errFile ? _errFile : "default", _out_error == 1 ? _errFile : "default"
           ,_background ? "YES" : "NO");
    printf("\n\n");
}
static void connectFd(int source, int destination) {
    if (source == destination) { return; }

    if (dup2(source, destination) < 0) { perror("dup2"); _exit(1); }
}

static void closeFiles(int inputFd, int outputFd, int errorFd) {
    if (inputFd > 2) { close(inputFd); }

    if (outputFd > 2 && outputFd != inputFd) { close(outputFd); }

    if (errorFd > 2 && errorFd != inputFd && errorFd != outputFd) { close(errorFd); }
}

void Command::execute() {
    if (_numberOfSimpleCommands == 0) { clear(); return; }

    if (_numberOfSimpleCommands == 1 &&
        strcmp(_simpleCommands[0]->_arguments[0], "exit") == 0) {
        printf("Good bye!!\n");
        clear();
        exit(0);
    }

    if (_numberOfSimpleCommands == 1 &&
        strcmp(_simpleCommands[0]->_arguments[0], "cd") == 0) {
        SimpleCommand *command = _simpleCommands[0];

        if (command->_numberOfArguments > 2) { fprintf(stderr, "cd: too many arguments\n"); clear(); return; }

        const char *directory;

        if (command->_numberOfArguments == 1) { directory = getenv("HOME"); }
        else { directory = command->_arguments[1]; }

        if (directory == NULL) { fprintf(stderr, "cd: HOME is not set\n"); }
        else if (chdir(directory) < 0) { perror("cd"); }

        clear();
        return;
    }

    if (_background) { fprintf(stderr, "Background execution will be added next\n"); clear(); return; }

    int inputFd = STDIN_FILENO;
    int outputFd = STDOUT_FILENO;
    int errorFd = STDERR_FILENO;
    bool valid = true;

    if (_inputFile != NULL) {
        inputFd = open(_inputFile, O_RDONLY);

        if (inputFd < 0) { perror(_inputFile); valid = false; }
    }

    if (valid && _outFile != NULL) {
        int flags = O_WRONLY | O_CREAT;

        if (_append) { flags |= O_APPEND; }
        else { flags |= O_TRUNC; }

        outputFd = open(_outFile, flags, 0666);

        if (outputFd < 0) { perror(_outFile); valid = false; }
    }

    if (valid && _errFile != NULL) {
        if (_out_error) { errorFd = outputFd; }
        else {
            errorFd = open(_errFile,
                           O_WRONLY | O_CREAT | O_TRUNC, 0666);

            if (errorFd < 0) { perror(_errFile); valid = false; }
        }
    }

    if (!valid) { closeFiles(inputFd, outputFd, errorFd); clear(); return; }

    std::vector<pid_t> children;
    int previousFd = inputFd;

    fflush(NULL);

    for (int i = 0; i < _numberOfSimpleCommands; i++) {
        int pipeFd[2];
        bool lastCommand = (i == _numberOfSimpleCommands - 1);

        if (!lastCommand) {
            if (pipe(pipeFd) < 0) { perror("pipe"); valid = false; break; }
        }

        pid_t pid = fork();

        if (pid < 0) {
            perror("fork");

            if (!lastCommand) { close(pipeFd[0]); close(pipeFd[1]); }

            valid = false;
            break;
        }

        if (pid == 0) {
            // The child must respond normally to Ctrl+C.
            struct sigaction action = {};
            action.sa_handler = SIG_DFL;
            sigemptyset(&action.sa_mask);

            if (sigaction(SIGINT, &action, NULL) < 0) { perror("sigaction"); _exit(1); }

            connectFd(previousFd, STDIN_FILENO);

            if (lastCommand) { connectFd(outputFd, STDOUT_FILENO); }
            else { connectFd(pipeFd[1], STDOUT_FILENO); }

            connectFd(errorFd, STDERR_FILENO);

            closeFiles(inputFd, outputFd, errorFd);

            if (previousFd > 2 && previousFd != inputFd) { close(previousFd); }

            if (!lastCommand) { close(pipeFd[0]); close(pipeFd[1]); }

            execvp(_simpleCommands[i]->_arguments[0],
                   _simpleCommands[i]->_arguments);

            perror(_simpleCommands[i]->_arguments[0]);
            _exit(127);
        }

        children.push_back(pid);

        if (previousFd > 2 && previousFd != inputFd) { close(previousFd); }

        if (!lastCommand) { close(pipeFd[1]); previousFd = pipeFd[0]; }
        else { previousFd = -1; }
    }

    if (previousFd > 2 && previousFd != inputFd) { close(previousFd); }

    closeFiles(inputFd, outputFd, errorFd);

    if (!valid) {
        for (int i = 0; i < (int)children.size(); i++) { kill(children[i], SIGKILL); }
    }

    for (int i = 0; i < (int)children.size(); i++) {
        int status;
        pid_t result;

        do {
            result = waitpid(children[i], &status, 0);
        }
        while (result < 0 && errno == EINTR);

        if (result < 0) { perror("waitpid"); }
    }

    clear();
}
void Command::prompt() {

    std::string input;
    while (true) {
        printf("myshell>");
        fflush(stdout);
        if (!std::getline(std::cin, input)) { break; }
        std::vector<Token> tokens = tokenize(input);
        parse(tokens);
    }
}

Command Command::_currentCommand;
SimpleCommand *Command::_currentSimpleCommand;

int main() {
    struct sigaction action = {};
    action.sa_handler = SIG_IGN;
    sigemptyset(&action.sa_mask);

    if (sigaction(SIGINT, &action, NULL) < 0) { perror("sigaction"); return 1; }

    Command::_currentCommand.prompt();
    return 0;
}
