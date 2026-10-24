#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <glob.h>
#include <vector>
#include <string>
#include <cctype>
#include <iostream>
#include "command.h"
#include "tokenizer.h"
#include <errno.h>
static int g_log_fd = -1;

static void log_term() {
	const char msg[] = "Child terminated\n";
	int w = 0;
	int len = sizeof(msg) - 1;
	while(w < len) {
		ssize_t res = write(g_log_fd, msg + w, len - w);
		if(res > 0) {
			w += res;
		} else if(res < 0 && errno == EINTR) {
			continue; 
		} else {
			break;
		}
	}
}

static void chld_handler(int) {
	int old_err = errno;
	while (true) {
		pid_t p = waitpid(-1, NULL, WNOHANG);
		if (p > 0) log_term();
		else if (p < 0 && errno == EINTR) continue;
		else break;
	}
	errno = old_err;
}

void parse(std::vector<Token> &toks) {
	Command cmd_obj;
	SimpleCommand* cur_cmd = new SimpleCommand();
	cmd_obj.insertSimpleCommand(cur_cmd);
	bool ok = true;
	int t_sz = (int)toks.size();
	int i = 0;
	while (i < t_sz && ok) {
		TokenType t = toks[i].type;
		if(t == TOKEN_EOF) break;
		if (t == TOKEN_PIPE) {
			if (cur_cmd->_numberOfArguments == 0) {
				fprintf(stderr, "Missing command before pipe\n");
				ok = false;
			} else {
				cur_cmd = new SimpleCommand();
				cmd_obj.insertSimpleCommand(cur_cmd);
			}
			i++;
			continue;
		}
		if (t == TOKEN_BACKGROUND) {
			if(cur_cmd->_numberOfArguments == 0 || i + 1 >= t_sz || toks[i + 1].type != TOKEN_EOF) {
				fprintf(stderr, "& must be at the end\n");
				ok = false;
			} else {
				cmd_obj._background = 1;
			}
			i++;
			continue;
		}
		bool is_redir = (t == TOKEN_INPUT || t == TOKEN_REDIRECT || t == TOKEN_APPEND ||
		t == TOKEN_ERROR || t == TOKEN_REDIRECT_AND_ERROR);
		if(is_redir) {
			if(i + 1 >= t_sz || (toks[i + 1].type != TOKEN_COMMAND && toks[i + 1].type != TOKEN_ARGUMENT)) {
				fprintf(stderr, "Missing filename after redirection\n");
				ok = false;
				break;
			}
			bool is_dup = false;
			if(t == TOKEN_INPUT) is_dup = (cmd_obj._inputFile != NULL);
			else if(t == TOKEN_ERROR) is_dup = (cmd_obj._errFile != NULL);
			else if(t == TOKEN_REDIRECT_AND_ERROR) is_dup = (cmd_obj._outFile != NULL || cmd_obj._errFile != NULL);
			else is_dup = (cmd_obj._outFile != NULL);
			if(is_dup) {
				fprintf(stderr, "Duplicate redirection\n");
				ok = false;
				break;
			}
			i++;
		} else if(t != TOKEN_COMMAND && t != TOKEN_ARGUMENT) {
			fprintf(stderr, "Invalid token\n");
			ok = false;
			break;
		}
		std::string raw_str, pat;
		const std::string &raw = toks[i].value;
		char q_char = 0;
		bool has_wild = false;
		int j = 0;
		while (j < (int)raw.size()) {
			char c = raw[j];
			bool esc = false;
			if(c == '\\' && q_char != '\'') {
				if(j + 1 >= (int)raw.size()) {
					fprintf(stderr, "Trailing backslash\n");
					ok = false;
					break;
				}
				char nxt = raw[j + 1];
				if(q_char == '"' && nxt != '"' && nxt != '\\' && nxt != '$' && nxt != '`') {
					raw_str += c;
					pat += "\\\\";
					j++; continue;
				}
				c = raw[++j];
				esc = true;
			}
			else if(q_char && c == q_char) { q_char = 0; j++; continue; }
			else if(!q_char && (c == '\'' || c == '"')) { q_char = c; j++; continue; }
			raw_str += c;
			if((q_char || esc) && strchr("*?[]\\", c)) pat += '\\';
			pat += c;
			if(!q_char && !esc && (c == '*' || c == '?')) has_wild = true;
			j++;
		}
		if(q_char) {
			fprintf(stderr, "Unmatched quote\n");
			ok = false;
		}
		if(!ok) break;
		glob_t matches = {};
		int g_res = GLOB_NOMATCH;
		if(has_wild) g_res = glob(pat.c_str(), 0, NULL, &matches);
		if(g_res != 0 && g_res != GLOB_NOMATCH) {
			fprintf(stderr, "Wildcard expansion failed\n");
			ok = false;
		} else if(is_redir && g_res == 0 && matches.gl_pathc != 1) {
			fprintf(stderr, "Redirection must match exactly one file\n");
			ok = false;
		} else if(!is_redir) {
			if(g_res == 0) {
				size_t j = 0;
				while (j < matches.gl_pathc) {
					cur_cmd->insertArgument(strdup(matches.gl_pathv[j]));
					j++;
				}
			} else {
				cur_cmd->insertArgument(strdup(raw_str.c_str()));
			}
		} else {
			if(g_res == 0) raw_str = matches.gl_pathv[0];
			if(raw_str.empty()) {
				fprintf(stderr, "Empty redirection filename\n");
				ok = false;
			} else if (t == TOKEN_INPUT) {
				cmd_obj._inputFile = strdup(raw_str.c_str());
			} else if (t == TOKEN_ERROR) {
				cmd_obj._errFile = strdup(raw_str.c_str());
			} else {
				cmd_obj._outFile = strdup(raw_str.c_str());
				cmd_obj._append = (t != TOKEN_REDIRECT);
				if(t == TOKEN_REDIRECT_AND_ERROR) {
					cmd_obj._errFile = strdup(raw_str.c_str());
					cmd_obj._out_error = 1;
				}
			}
		}
		if(has_wild) globfree(&matches);
		++i;
	}
	if(ok && cur_cmd->_numberOfArguments == 0) {
		if(cmd_obj._numberOfSimpleCommands > 1 || cmd_obj._inputFile || cmd_obj._outFile || cmd_obj._errFile) {
			fprintf(stderr, "Missing command\n");
		}
		ok = false;
	}
	if(ok) cmd_obj.execute();
	else cmd_obj.clear();
	free(cmd_obj._simpleCommands);
}

SimpleCommand::SimpleCommand() {
	_numberOfAvailableArguments = 5;
	_numberOfArguments = 0;
	_arguments = (char **)malloc(_numberOfAvailableArguments * sizeof(char *));
	_arguments[0] = NULL;
}

void SimpleCommand::insertArgument(char *a) {
	if(_numberOfAvailableArguments == _numberOfArguments + 1) {
		_numberOfAvailableArguments *= 2;
		_arguments = (char **)realloc(_arguments, _numberOfAvailableArguments * sizeof(char *));
	}
	_arguments[_numberOfArguments] = a;
	_arguments[_numberOfArguments + 1] = NULL;
	_numberOfArguments++;
}

Command::Command() {
	_numberOfAvailableSimpleCommands = 1;
	_simpleCommands = (SimpleCommand **) malloc(_numberOfAvailableSimpleCommands * sizeof(SimpleCommand *));
	_numberOfSimpleCommands = 0;
	_outFile = _inputFile = _errFile = NULL;
	_background = _append = _out_error = 0;
}

void Command::insertSimpleCommand(SimpleCommand *sc) {
	if(_numberOfAvailableSimpleCommands == _numberOfSimpleCommands) {
		_numberOfAvailableSimpleCommands *= 2;
		_simpleCommands = (SimpleCommand **)realloc(_simpleCommands, _numberOfAvailableSimpleCommands * sizeof(SimpleCommand *));
	}
	_simpleCommands[_numberOfSimpleCommands] = sc;
	_numberOfSimpleCommands++;
}

void Command::clear() {
	int i = 0;
	while (i < _numberOfSimpleCommands) {
		int j = 0;
		while (j < _simpleCommands[i]->_numberOfArguments) {
			free(_simpleCommands[i]->_arguments[j]);
			j++;
		}
		free(_simpleCommands[i]->_arguments);
		delete _simpleCommands[i];
		i++;
	}
	free(_outFile);
	free(_inputFile);
	free(_errFile);
	_numberOfSimpleCommands = 0;
	_outFile = _inputFile = _errFile = NULL;
	_background = _append = _out_error = 0;
}

void Command::print() {
	printf("\nCOMMAND TABLE\n");
	int i = 0;
	while (i < _numberOfSimpleCommands) {
		printf("%d: ", i);
		int j = 0;
		while (j < _simpleCommands[i]->_numberOfArguments) {
			printf("\"%s\" ", _simpleCommands[i]->_arguments[j]);
			j++;
		}
		printf("\n");
		i++;
	}
	printf("Input: %s  Output: %s  Error: %s\n",
	_inputFile ? _inputFile : "default",
	_outFile ? _outFile : "default",
	_errFile ? _errFile : "default");
	printf("Append: %s  Err&Out: %s  Background: %s\n\n",
	_append ? "YES" : "NO",
	_out_error ? "YES" : "NO",
	_background ? "YES" : "NO");
}

void Command::execute() {
	if(_numberOfSimpleCommands == 0) {
		clear();
		return;
	}
	if(isatty(STDIN_FILENO)) print();
	if(_numberOfSimpleCommands == 1 && strcmp(_simpleCommands[0]->_arguments[0], "exit") == 0) {
		printf("Good bye!!\n");
		clear();
		exit(0);
	}
	if(_numberOfSimpleCommands == 1 && strcmp(_simpleCommands[0]->_arguments[0], "cd") == 0) {
		SimpleCommand *sc = _simpleCommands[0];
		if(sc->_numberOfArguments > 2) {
			fprintf(stderr, "cd: too many arguments\n");
			clear();
			return;
		}
		const char *dir = (sc->_numberOfArguments == 1) ? getenv("HOME") : sc->_arguments[1];
		if(!dir) fprintf(stderr, "cd: HOME is not set\n");
		else if(chdir(dir) < 0) perror("cd");
		clear();
		return;
	}
	int fds[3] = {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO};
	const char *files[3] = {_inputFile, _outFile, _errFile};
	bool ok = true;
	int i = 0;
	while (i < 3) {
		if(i == 2 && _out_error) {
			fds[2] = fds[1];
			i++;
			continue;
		}
		if(!files[i]) { i++; continue; }
		int f = O_RDONLY;
		if(i != 0) {
			f = O_WRONLY | O_CREAT;
			f |= (i == 1 && _append) ? O_APPEND : O_TRUNC;
		}
		fds[i] = open(files[i], f, 0666);
		if(fds[i] < 0) {
			perror(files[i]);
			ok = false;
			break;
		}
		i++;
	}
	sigset_t c_sigs, old_set;
	sigemptyset(&c_sigs);
	sigaddset(&c_sigs, SIGCHLD);
	if(ok && sigprocmask(SIG_BLOCK, &c_sigs, &old_set) < 0) {
		perror("sigprocmask");
		ok = false;
	}
	if(!ok) {
		int i = 0;
		while (i < 3) {
			if(fds[i] > 2 && (i != 2 || fds[i] != fds[1])) close(fds[i]);
			i++;
		}
		clear();
		return;
	}
	std::vector<pid_t> pids;
	int prev_pipe = fds[0];
	fflush(NULL);
	i = 0;
	while (i < _numberOfSimpleCommands) {
		int p_fd[2];
		bool is_last = (i == _numberOfSimpleCommands - 1);
		if(!is_last && pipe(p_fd) < 0) {
			perror("pipe");
			ok = false;
			break;
		}
		pid_t p = fork();
		if(p < 0) {
			perror("fork");
			if(!is_last) {
				close(p_fd[0]);
				close(p_fd[1]);
			}
			ok = false;
			break;
		}
		if(p == 0) {
			struct sigaction sa = {};
			sa.sa_handler = SIG_DFL;
			sigemptyset(&sa.sa_mask);
			if(sigaction(SIGINT, &sa, NULL) < 0 || sigaction(SIGCHLD, &sa, NULL) < 0) {
				perror("sigaction");
				_exit(1);
			}
			if(sigprocmask(SIG_SETMASK, &old_set, NULL) < 0) {
				perror("sigprocmask");
				_exit(1);
			}
			int src[3] = {prev_pipe, fds[1], fds[2]};
			if(!is_last) src[1] = p_fd[1];
			int j = 0;
			while (j < 3) {
				if(src[j] != j && dup2(src[j], j) < 0) {
					perror("dup2");
					_exit(1);}
				j++;}
			j = 0;
			while (j < 3) {
				if(fds[j] > 2 && (j != 2 || fds[j] != fds[1])) close(fds[j]);
				j++;
			}
			if(prev_pipe > 2 && prev_pipe != fds[0]) close(prev_pipe);
			if(!is_last) {
				close(p_fd[0]);
				close(p_fd[1]);
			}
			execvp(_simpleCommands[i]->_arguments[0], _simpleCommands[i]->_arguments);
			perror(_simpleCommands[i]->_arguments[0]);
			_exit(127);
		}
		pids.push_back(p);
		if(prev_pipe > 2 && prev_pipe != fds[0]) close(prev_pipe);
		if(!is_last) {
			close(p_fd[1]);
			prev_pipe = p_fd[0];
		} else {
			prev_pipe = -1;
		}
		i++;
	}
	if(prev_pipe > 2 && prev_pipe != fds[0]) close(prev_pipe);
	i = 0;
	while (i < 3) {
		if(fds[i] > 2 && (i != 2 || fds[i] != fds[1])) close(fds[i]);
		i++;
	}
	if(!ok) {
		int i = 0;
		while (i < (int)pids.size()) {
			kill(pids[i], SIGKILL);
			i++;}
	}
	if(sigprocmask(SIG_SETMASK, &old_set, NULL) < 0) {
		perror("sigprocmask");
		exit(1);
	}
	if(!_background || !ok) {
		int i = 0;
		while (i < (int)pids.size()) {
			pid_t res;
			do {
				res = waitpid(pids[i], NULL, 0);
			} while(res < 0 && errno == EINTR);
			if(res > 0) log_term();
			else if(res < 0 && errno != ECHILD) perror("waitpid");
			i++;
		}
	} else if(!pids.empty()) {
		printf("[background] %ld\n", (long)pids.back());
	}
	clear();}

void Command::prompt() {
	std::string raw_line;
	while (true) {
		printf("myshell>");
		fflush(stdout);
		if(!std::getline(std::cin, raw_line)) break;
		std::vector<Token> t_vec;
		bool ok = true;
		int p = 0;
		int len = (int)raw_line.size();
		while (p < len) {
			if(std::isspace((unsigned char)raw_line[p])) {
				p++;
				continue;}
			Token tk;
			int s_idx = p;
			if(raw_line.compare(p, 3, ">>&") == 0) {
				tk.type = TOKEN_REDIRECT_AND_ERROR; p += 3;
			}
			else if(raw_line.compare(p, 2, ">>") == 0) {
				tk.type = TOKEN_APPEND; p += 2;}
			else if(raw_line.compare(p, 2, "2>") == 0) {
				tk.type = TOKEN_ERROR; p += 2;}
			else if(raw_line[p] == '>') { tk.type = TOKEN_REDIRECT; p++; }
			else if(raw_line[p] == '<') { tk.type = TOKEN_INPUT; p++; }
			else if(raw_line[p] == '|') { tk.type = TOKEN_PIPE; p++; }
			else if(raw_line[p] == '&') { tk.type = TOKEN_BACKGROUND; p++; }
			else {
				tk.type = TOKEN_ARGUMENT;
				char q_char = 0;
				while (p < len) {
					char c = raw_line[p];
					if(c == '\\' && q_char != '\'') {
						if(p + 1 >= len) {
							fprintf(stderr, "Trailing backslash\n");
							ok = false;
							break;
						}
						p += 2;
						continue;
					}
					if(q_char) {
						if(c == q_char) q_char = 0;
					} else if(c == '\'' || c == '"') {
						q_char = c;
					} else if(std::isspace((unsigned char)c) || c == '|' || c == '<' || c == '>' || c == '&') {
						break;}
					p++;
				}
				if(!ok) break;
				if(q_char) {
					fprintf(stderr, "Unmatched quote\n");
					ok = false;
					break;
				}}
			tk.value = raw_line.substr(s_idx, p - s_idx);
			t_vec.push_back(tk);}
		if(ok) {
			t_vec.push_back({TOKEN_EOF, ""});
			parse(t_vec);
		}
	}}
Command Command::_currentCommand;
SimpleCommand *Command::_currentSimpleCommand;

int main() {
	g_log_fd = open("shell.log", O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
	if(g_log_fd < 0) {
		perror("shell.log");
		return 1;
	}
	struct sigaction sa = {};
	sa.sa_handler = SIG_IGN;
	sigemptyset(&sa.sa_mask);
	if(sigaction(SIGINT, &sa, NULL) < 0) {
		perror("sigaction");
		close(g_log_fd);
		return 1;
	}
	sa.sa_handler = chld_handler;
	sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
	if(sigaction(SIGCHLD, &sa, NULL) < 0) {
		perror("sigaction");
		close(g_log_fd);
		return 1;
	}
	Command::_currentCommand.prompt();
	return 0;
}
