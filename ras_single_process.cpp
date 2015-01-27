#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <cinttypes>
#include <cerrno>
#include <string>
#include <vector>
#include <map>

#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>

#include "socket.h"
#include "io_wrapper.h"
#include "parser.h"
#include "pipe_manager.h"
#include "cstring_more.h"
#include "server_arch.h"
#include "number_pool.h"
#include "user_communicate_manager.h"

const char RWG_IP[] = "0.0.0.0";
const int RWG_DEFAULT_PORT = 53000;
const int MAX_ONELINE_CMD_SIZE = 65536;
const int MAX_CMD_SIZE = 256;
const int MAX_USER = 30;


struct UserRecordSingleProcess{
    SocketAddr user_addr;
    int user_id;
    std::string user_name;
    socketfd_t user_socket;
    std::map<std::string, std::string> environ;
    char command_buf[MAX_ONELINE_CMD_SIZE];
    int command_size;
    PipeManager cmd_pipe_manager;

    UserRecordSingleProcess();
    UserRecordSingleProcess(SocketAddr user_addr, socketfd_t user_socket, int user_id);
    void use_env();
};

std::vector<UserRecordSingleProcess> all_client_record;
UserCommunicationManager user_communicate_manager(MAX_USER, "fifo/");

bool user_id_exist(const std::vector<UserCommunicationManager>& all_client_record, int client_id){
    for( auto& one_client : all_client_record ){
        if( one_client.user_id == client_id ){
            return true;
        }
    }
    return false;
}

void rwg_tell(int sender_counter, int client_id, std::string message){
    /*
    command:
            % tell (client id) (message)

    received:
            [id (client id)]
            *** (sender's name) told you ***: (message)

            [id (sender)]
    [Error] *** Error: user #(client id) does not exist yet. *** 
    */
    UserRecordSingleProcess& sender = all_client_record[sender_counter];
    for( auto& one_client : all_client_record ){
        if( one_client.user_id == client_id ){
            std::string send_msg = "*** " + sender.user_name + " told you ***: " + message + "\n";
            write_all_str(one_client.user_socket, send_msg);
            return;
        }
    }

    // Error: no user client_id
    std::string error_msg = "*** Error: user #" + std::to_string(client_id) + " does not exist yet. ***\n";
    write_all_str(sender.user_socket, error_msg);
}

void rwg_yell(int sender_counter, std::string message){
    /*
    command:
            % yell (message)

    received:
            [id all]
            *** (sender's name) yelled ***: (message)
    */
    UserRecordSingleProcess& sender = all_client_record[sender_counter];
    std::string send_msg = "*** " + sender.user_name + " yell ***: " + message + "\n";

    for( auto& one_client : all_client_record ){
        write_all_str(one_client.user_socket, send_msg);
    }
}

void rwg_name(int sender_counter, std::string name){
    /*
    command:
            % name (name)

    received:
            [id all]
            *** User from (IP/port) is named '(name)'. ***
            [id (sender)]
    [Error] *** User '(name)' already exists. ***
    */
    UserRecordSingleProcess& sender = all_client_record[sender_counter];


    std::string error_msg = "*** User '" + name + "' already exists. ***\n";
    for( auto& one_client : all_client_record ){
        if( one_client.user_name == name ){
            // Error: same name
            write_all_str(sender.user_socket, error_msg);
            return;
        }
    }
    // Correct: no same name
    sender.user_name = name;
    // boardcast
    std::string send_msg =  "*** User from " + sender.user_addr.to_str("/") + " is named '" + name + "'. ***\n";
    for( auto& one_client : all_client_record ){
        write_all_str(one_client.user_socket, send_msg);   
    }
}

void rwg_who(int sender_counter){
    /*
    command:
            % who
    
    received:
            [id (sender)]
            <ID>[Tab]<nickname>[Tab]<IP/port>[Tab]<indicate me>
            (1st id)[Tab](1st name)[Tab](1st IP/port)([Tab](<-me))
            (2nd id)[Tab](2nd name)[Tab](2nd IP/port)([Tab](<-me))
            (3rd id)[Tab](3rd name)[Tab](3rd IP/port)([Tab](<-me))
            ...
    */
    UserRecordSingleProcess& sender = all_client_record[sender_counter];
    write_all_str(sender.user_socket, "<ID>\t<nickname>\t<IP/port>\t<indicate me>\n");

    for( auto& one_client : all_client_record ){
        std::string send_msg = std::to_string(one_client.user_id) + "\t" + one_client.user_name + "\t" + \
                               one_client.user_addr.to_str("/");
        if( one_client.user_id == sender.user_id ){
            send_msg += "\t<-me";
        }
        send_msg += "\n";
        
        write_all_str(sender.user_socket, send_msg);
    }
}

void rwg_enter_room_msg(int enter_client_counter){
    /* 
     * entering room
     * received:
     *         [id all]
     *         *** User '(no name)' entered from 140.113.215.63/1013. ***
     */
    UserRecordSingleProcess& enter_client = all_client_record[enter_client_counter];
    std::string send_msg = "*** User '(no name)' entered from " + enter_client.user_addr.to_str("/") + ". ***\n";

    for( auto& one_client : all_client_record ){
        write_all_str(one_client.user_socket, send_msg);
    }
}

void rwg_leave_room_msg(int leave_client_counter){
    /* 
     * leave room
     * received:
     *         [id all]
     *         *** User '(name)' left ***
     */
    UserRecordSingleProcess& leave_client = all_client_record[leave_client_counter];
    std::string send_msg = "*** User '" + leave_client.user_name + "' left ***\n";

    for( auto& one_client : all_client_record ){
        write_all_str(one_client.user_socket, send_msg);
    }
}

int ras_service_single_process(int client_counter);
int execute_cmd(socketfd_t client_socket, PipeManager& cmd_pipe_manager, const char* origin_command, 
  int client_counter);
    const int CMD_NORMAL = 0, CMD_EXIT = 1;

/* ras_service sub functions */
void ras_shell_init_single_process();
void initial_env(std::map<std::string, std::string>& environ);
void print_welcome_msg(socketfd_t client_socket);
void check_command_full(int client_socket, char* command, int& current_size, int max_size);
int read_to_buf_max_size(int fd, char* buf, int& current_size, int max_size);

/* execute_cmd sub functions */
int pre_fd_redirection(PipeManager& cmd_pipe_manager, int origin_fd, Redirection& redirect_obj, int sender_counter);
int fd_redirection(PipeManager& cmd_pipe_manager, int origin_fd, Redirection& redirect_obj,
  AnonyPipe& child_output_pipe, int sender_counter);
bool is_internal_command_and_run(bool& is_exit, SingleCommand& cmd, socketfd_t client_socket, 
  std::map<std::string, std::string>& environ);
bool is_rwg_command_and_run(SingleCommand& cmd, int sender_counter);
void processing_child_output_data(AnonyPipe& child_output_pipe, socketfd_t client_socket);

int main(int argc, char** argv){
    SocketAddr rwg_addr(RWG_IP, RWG_DEFAULT_PORT);
    if( argc == 2 ){
        rwg_addr.port_hbytes = strtol(argv[1], NULL, 0);
    }
    
    socketfd_t rwg_listen_socket = bind_and_listen_tcp_socket(rwg_addr);

    // global initialization
    ras_shell_init_single_process(); // chdir
    number_pool id_pool(1, 30);

    fd_set read_fds;
    int max_fd;
    FD_ZERO(&read_fds);
    FD_SET(rwg_listen_socket, &read_fds);
    max_fd = rwg_listen_socket + 1;

    /* The routine of every client (all in one thread)
     * 1. new client initialization
     *
     * loop:
     *    2. client input
     *    3. processing client command and response
     *    4. do next command preparation (ex. print shell prompt.)
     * 
     * 5. client finalization
     */
    while( 1 ){
        fd_set select_read_fds = read_fds;
        if( select(max_fd, &select_read_fds, NULL, NULL, NULL) < 0 ){
            perror_and_exit("select error");
        }

        if( FD_ISSET(rwg_listen_socket, &select_read_fds) ){
            SocketAddr client_addr;
            socketfd_t client_socket = socket_accept(rwg_listen_socket, client_addr);

            all_client_record.emplace_back(client_addr, client_socket, id_pool.get_min_idle_id());
            FD_SET(client_socket, &read_fds);
            if( max_fd < client_socket+1 ){
                max_fd = client_socket+1;
            }
            // 1. new client initialization
            int last = all_client_record.size() - 1;
            print_welcome_msg(all_client_record[last].user_socket);
            rwg_enter_room_msg(last);
            write_all(all_client_record[last].user_socket, "% ", 2);
        }

        for( int client_counter=0; client_counter < all_client_record.size(); client_counter++){
            if( FD_ISSET(all_client_record[client_counter].user_socket, &select_read_fds) ){ // 2. client input
                // 3. processing client command and response.
                int error = ras_service_single_process(client_counter);
                if( error == -1 ){
                    // 5. finalization(clear) this client.
                    rwg_leave_room_msg(client_counter);

                    FD_CLR(all_client_record[client_counter].user_socket, &read_fds);
                    close(all_client_record[client_counter].user_socket);

                    id_pool.release_id(all_client_record[client_counter].user_id);

                    // erase this client in all_client_record.
                    all_client_record.erase(all_client_record.begin() + client_counter); 
                    client_counter--; // not client_counter++ in increment stage of for loop.
                }
                else{           
                    // 4. do next command preparation (ex. print shell prompt.)
                    write_all(all_client_record[client_counter].user_socket, "% ", 2);
                }
            }
        }
    }
    return 0;
}

int ras_service_single_process(int client_counter){
    /*
     * return value:
     *  0 for normal
     * -1 for ending
     */
    UserRecordSingleProcess& one_client_record = all_client_record[client_counter];
    one_client_record.use_env();

    check_command_full(one_client_record.user_socket, one_client_record.command_buf, 
      one_client_record.command_size, MAX_ONELINE_CMD_SIZE);

    int recv_size = read_to_buf_max_size(one_client_record.user_socket, one_client_record.command_buf,
      one_client_record.command_size, MAX_ONELINE_CMD_SIZE);

    if(recv_size == 0)
        return -1;

    char* cur_cmd_head = one_client_record.command_buf;
    char* newline_char;
    while( (newline_char = strchr(cur_cmd_head, '\n')) != NULL ){
        /* split command and execute it. */
        newline_char[0] = '\0';
        if( execute_cmd(one_client_record.user_socket, one_client_record.cmd_pipe_manager,
                        cur_cmd_head, client_counter) == CMD_EXIT )
            return -1;
        cur_cmd_head = newline_char+1;
    }

    if(cur_cmd_head != one_client_record.command_buf){
        /* move the un-executed command to start position of cmd_buf. */
        int used_byte = cur_cmd_head - one_client_record.command_buf;
        one_client_record.command_size -= used_byte;
        memmove(one_client_record.command_buf, cur_cmd_head, one_client_record.command_size);
    }

    return 0;
}


int execute_cmd(socketfd_t client_socket, PipeManager& cmd_pipe_manager, const char* origin_command,
  int client_counter){
    /* parsing and execute shell command */
    int cmd_len = strlen(origin_command);
    if( cmd_len == 0 ) 
        return CMD_NORMAL;

    string command(origin_command);

    /* parsing */
    OneLineCommand parsed_cmds;
    parsed_cmds.parse_one_line_cmd(command);
    parsed_cmds.print();

    /* processing command */
    bool is_exit = false;
    std::map<std::string, std::string>& environ = all_client_record[client_counter].environ;
    bool is_internal = is_internal_command_and_run(is_exit, parsed_cmds.cmds[0], client_socket, environ);
    if( is_exit ) return CMD_EXIT;
    if( is_internal ) return CMD_NORMAL;

    bool is_rwg = is_rwg_command_and_run(parsed_cmds.cmds[0], client_counter);
    if( is_rwg ) return CMD_NORMAL;

    // cmd_pipe_manager, parsed_cmds
    AnonyPipe child_output_pipe;
    child_output_pipe.create_pipe();
    int legal_cmd = 0;

    for( auto& current_cmd : parsed_cmds.cmds ){
        /* 
         * exec(current_cmd.executable, current_cmd.gen_argv())
         * stdin: current_cmd.std_input.(kind, data), cmd_pipe_manager.cmd_has_pipe(0)
         * stdout: current_cmd.std_output.(kind, data), 
         * stderr: current_cmd.std_error.(kind, data), 
         * (any redirect to pipe) cmd_pipe_manager
         */

        /* pre-processing redirection in parent process */
        if( current_cmd.std_input.kind != REDIR_NONE && cmd_pipe_manager.cmd_has_pipe(0) ){
            error_print_and_exit("ambiguous input redirection");
        }
        if( cmd_pipe_manager.cmd_has_pipe(0) ){
            current_cmd.std_input.set_pipe_redirect(0);
        }
        int err1 = pre_fd_redirection(cmd_pipe_manager, STDIN_FILENO, current_cmd.std_input, client_counter);
        int err2 = pre_fd_redirection(cmd_pipe_manager, STDOUT_FILENO, current_cmd.std_output, client_counter);
        int err3 = pre_fd_redirection(cmd_pipe_manager, STDERR_FILENO, current_cmd.std_error, client_counter);
        if( err1 == -1 || err2 == -1 || err3 == -1 ){
            return CMD_NORMAL;
        }


        int pid = fork();
        if( pid == 0 ){
            /* stdin redirection */
            fd_redirection(cmd_pipe_manager, STDIN_FILENO, current_cmd.std_input, child_output_pipe);
            /* stdout redirection */
            fd_redirection(cmd_pipe_manager, STDOUT_FILENO, current_cmd.std_output, child_output_pipe);
            /* stderr redirection */
            fd_redirection(cmd_pipe_manager, STDERR_FILENO, current_cmd.std_error, child_output_pipe);

            char** argv = current_cmd.gen_argv();
            execvp(current_cmd.executable.c_str(), argv);
            /* exec error: print "Unknown command [command_name]" */
            char unknown_cmd[MAX_CMD_SIZE+128] = "";
            int u_cmd_size = snprintf(unknown_cmd, MAX_CMD_SIZE+128, "Unknown command: [%s].\n", current_cmd.executable.c_str());
            write(child_output_pipe.write_fd(), unknown_cmd, u_cmd_size);
            exit(EXIT_FAILURE);
        }
        else if(pid > 0){
            if( cmd_pipe_manager.cmd_has_pipe(0) ){
            /* if child stdin use pipe, close write end in parent. */
                cmd_pipe_manager.get_pipe(0).close_write();
            }
            int child_status;
            wait(&child_status);
            /* child status */
            if( WIFEXITED(child_status) ){
                int exit_status = WEXITSTATUS(child_status); 
                // printf("exit_status: %d\n", exit_status);
                if( exit_status == 0 ){
                    /* correct exit */
                    cmd_pipe_manager.get_pipe(0).close_pipe();
                    /* run the next command in one-line-command */
                    cmd_pipe_manager.next_pipe();
                }
                else{
                    /* error cmd, finish this one-line-command */
                    break;
                }
            }
        }   
        else{
            perror_and_exit("fork error");
        }
    }
    processing_child_output_data(child_output_pipe, client_socket);
    child_output_pipe.close_pipe();

    return CMD_NORMAL;
}

/* ras_service sub functions */
void ras_shell_init_single_process(){
    char ras_dir[1024+1];
    char* home_dir = getenv("HOME");
    if(!home_dir)
        error_print_and_exit("Error: No HOME enviroment variable\n");

    snprintf(ras_dir, 1024, "%s/ras/", home_dir);
    int ret = chdir(ras_dir);
    if(ret == -1)
        perror_and_exit("chdir error");
}

void initial_env(std::map<std::string, std::string>& environ){
    environ.clear();
    environ["PATH"] = "bin:.";
}

void print_welcome_msg(socketfd_t client_socket){
    char msg1[] = "****************************************\n";
    char msg2[] = "** Welcome to the information server. **\n";
    char msg3[] = "****************************************\n";
    /*
    char* msg4 = new char [65536];
    snprintf(msg4, 65535, "** You are in the directory, %s/ras/.\n", getenv("HOME"));
    char msg5[] = "** This directory will be under \"/\", in this system.\n";
    char msg6[] = "** This directory includes the following executable programs.\n";
    char msg7[] = "**\n";
    char msg8[] = "**    bin/\n";
    char msg9[] = "**    test.html    (test file)\n";
    char msg10[] = "**\n";
    char msg11[] = "** The directory bin/ includes:\n";
    char msg12[] = "**    cat\n";
    char msg13[] = "**    ls\n";
    char msg14[] = "**    removetag         (Remove HTML tags.)\n";
    char msg15[] = "**    removetag0        (Remove HTML tags with error message.)\n";
    char msg16[] = "**    number            (Add a number in each line.)\n";
    char msg17[] = "**\n";
    char msg18[] = "** In addition, the following two commands are supported by ras.\n";
    char msg19[] = "**    setenv\n";
    char msg20[] = "**    printenv\n";
    char msg21[] = "**\n";
    */

    write_all(client_socket, msg1, strlen(msg1));
    write_all(client_socket, msg2, strlen(msg2));
    write_all(client_socket, msg3, strlen(msg3));
    /*
    write_all(client_socket, msg4, strlen(msg4));
    write_all(client_socket, msg5, strlen(msg5));
    write_all(client_socket, msg6, strlen(msg6));
    write_all(client_socket, msg7, strlen(msg7));
    write_all(client_socket, msg8, strlen(msg8));
    write_all(client_socket, msg9, strlen(msg9));
    write_all(client_socket, msg10, strlen(msg10));
    write_all(client_socket, msg11, strlen(msg11));
    write_all(client_socket, msg12, strlen(msg12));
    write_all(client_socket, msg13, strlen(msg13));
    write_all(client_socket, msg14, strlen(msg14));
    write_all(client_socket, msg15, strlen(msg15));
    write_all(client_socket, msg16, strlen(msg16));
    write_all(client_socket, msg17, strlen(msg17));
    write_all(client_socket, msg18, strlen(msg18));
    write_all(client_socket, msg19, strlen(msg19));
    write_all(client_socket, msg20, strlen(msg20));
    write_all(client_socket, msg21, strlen(msg21));
    delete [] msg4;
    */
}

void check_command_full(int client_socket, char* command, int& current_size, int max_size){
    if(current_size == max_size){
        /* command too long */
        std::string err_msg = "command too long: \n" + std::string(command);
        write_all_str(client_socket, err_msg);
        error_print(err_msg.c_str());
        current_size = 0;
    }
}

int read_to_buf_max_size(int fd, char* buf, int& current_size, int max_size){
    /* read data from fd and write to buffer, the size of buffer isn't greater than max_size.
     * the buffer can have initial data store in it, initial data size should be passed by current_size.
     *
     * return: read size.
     */
    int recv_size = 0;
    recv_size = read(fd, buf+current_size, max_size-current_size);
    if( recv_size == 0 ){
        /* client is closing connection */
        return recv_size;
    }
    else if( recv_size == -1 ){
        perror_and_exit("read error");
    }
    current_size += recv_size; 
    buf[current_size] = '\0';
    return recv_size;
}

/* execute_cmd sub functions */
int pre_fd_redirection(PipeManager& cmd_pipe_manager, int origin_fd, Redirection& redirect_obj, int sender_counter){
    UserRecordSingleProcess& sender = all_client_record[sender_counter];
    /* create pipe */
    if( redirect_obj.kind == REDIR_PIPE ){
        if( origin_fd == STDOUT_FILENO || origin_fd == STDERR_FILENO ){
            int pipe_index = redirect_obj.data.pipe_index_in_manager;
            AnonyPipe& redirect_pipe = cmd_pipe_manager.get_pipe(pipe_index);
            if( !redirect_pipe.enable )
                redirect_pipe.create_pipe();
        }
    }
    else if( redirect_obj.kind == REDIR_TO_PERSON){
    /*
        pipe to (client id)
        received:
                [id all]
                *** (sender's name) (#(sender)) just piped '(command line)' to (client id's name) (#(client_id)) ***
                [id (sender)]
        [Error] *** Error: user #1 does not exist yet. *** 
        [Error2]*** Error: the pipe #(client id)->#(client id) already exists. *** 
                
        receive pipe from (id)
        received:
                [id all]
                *** (sender's name) (#(sender)) just received from (id's name) (#(id)) by '(command line)' ***
                [id (sender)]
        [Error] *** Error: the pipe #(client id)->#(client id) does not exist yet. *** 
    */
    /*
        if( origin_fd == STDIN_FILENO ){
            if( !user_communicate_manager.file_exist(sender.user_id, redirect_obj.data.person_id) ){
                // Error, no file to redirect;
                return -1;
            }
        }
        else{
            if( user_communicate_manager.file_exist(sender.user_id, redirect_obj.data.person_id) ){
                // Error, file is existed, can't redirect twice
                return -1;
            }
        }
    */
    }
    return 0;
}

void fd_redirection(PipeManager& cmd_pipe_manager, int origin_fd, Redirection& redirect_obj,
  AnonyPipe& child_output_pipe){
    /* do the STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO redirection */
    if( redirect_obj.kind == REDIR_NONE ){
        if( origin_fd == STDOUT_FILENO || origin_fd == STDERR_FILENO ){
            int output_fd = child_output_pipe.write_fd();
            dup2(output_fd, origin_fd);
        }
    }
    else if( redirect_obj.kind == REDIR_FILE ){
        int file_fd;
        if( origin_fd == STDIN_FILENO )
            file_fd = open(redirect_obj.data.filename.c_str(), O_RDONLY);
        else
            file_fd = open(redirect_obj.data.filename.c_str(), O_WRONLY|O_CREAT|O_TRUNC, 0644);

        if(file_fd == -1)
            perror_and_exit("open error");

        dup2(file_fd, origin_fd);
    }
    else if( redirect_obj.kind == REDIR_TO_PERSON){
        /*
        int file_fd;
        if( origin_fd == STDIN_FILENO )
            file_fd = open(redirect_obj.data.filename.c_str(), O_RDONLY);
        else
            file_fd = open(redirect_obj.data.filename.c_str(), O_WRONLY|O_CREAT|O_TRUNC, 0644);

        if(file_fd == -1)
            perror_and_exit("open error");

        dup2(file_fd, origin_fd);
        */
    }
    else if( redirect_obj.kind == REDIR_PIPE ){
        int pipe_index = redirect_obj.data.pipe_index_in_manager;
        AnonyPipe& redirect_pipe = cmd_pipe_manager.get_pipe(pipe_index);

        if( origin_fd == STDIN_FILENO ){
            int redirect_fd = redirect_pipe.read_fd();
            dup2(redirect_fd, origin_fd);
            redirect_pipe.close_pipe();
        }
        else if( origin_fd == STDOUT_FILENO || origin_fd == STDERR_FILENO ){
            int redirect_fd = redirect_pipe.write_fd();
            dup2(redirect_fd, origin_fd);
        }
    }
}

bool is_internal_command_and_run(bool& is_exit, SingleCommand& cmd, socketfd_t client_socket,
  std::map<std::string, std::string>& environ){
    if( cmd.executable == "exit" ){
        is_exit = true;
    }
    else if( cmd.executable == "printenv" ){
        char tmp[1024+1];
        const char* argv1 = cmd.arguments[1].c_str();
        int size = snprintf(tmp, 1024, "%s=%s\n", argv1, getenv(argv1));
        write_all(client_socket, tmp, size);
    }
    else if( cmd.executable == "setenv" ){
        const char* argv1 = cmd.arguments[1].c_str();
        const char* argv2 = cmd.arguments[2].c_str();
        setenv(argv1, argv2, 1);
        environ[argv1] = argv2;
    }
    else{
        return false;
    }
    return true;
}

bool is_rwg_command_and_run(SingleCommand& cmd, int sender_counter){
    if( cmd.executable == "tell" ){
        rwg_tell(sender_counter, std::stoi(cmd.arguments[1]), cmd.arguments[2]);
        return true;
    }
    else if( cmd.executable == "yell" ){
        rwg_yell(sender_counter, cmd.arguments[1]);
        return true;
    }
    else if( cmd.executable == "name" ){
        rwg_name(sender_counter, cmd.arguments[1]);
        return true;
    }
    else if( cmd.executable == "who" ){
        rwg_who(sender_counter);
        return true;
    }

    return false;
}

void processing_child_output_data(AnonyPipe& child_output_pipe, socketfd_t client_socket){
    child_output_pipe.close_write();
    while(1){
        char* read_buf[1024+1];
        int read_size = read(child_output_pipe.read_fd(), read_buf, 1024);
        if(read_size == 0){
            break; 
        }
        else if(read_size < 0){
            perror_and_exit("read child pipe error");
        }
        else{
            int write_size = write_all(client_socket, read_buf, read_size);
        }
    }
}

/* UserRecordSingleProcess */

UserRecordSingleProcess::UserRecordSingleProcess(){
    user_name = "no name";
    initial_env(environ);
    memset(command_buf, 0, MAX_ONELINE_CMD_SIZE);
    command_size = 0;
}

UserRecordSingleProcess::UserRecordSingleProcess(SocketAddr user_addr, socketfd_t user_socket, int user_id){
    user_name = "no name";
    this->user_addr = user_addr;
    this->user_socket = user_socket;
    this->user_id = user_id;
    initial_env(environ);
    memset(command_buf, 0, MAX_ONELINE_CMD_SIZE);
    command_size = 0;
}

void UserRecordSingleProcess::use_env(){
     clearenv();
     for (auto& kv : environ) {
         setenv(kv.first.c_str(), kv.second.c_str(), 1);
     }
     std::cout << "initial env: PATH = " << getenv("PATH") << std::endl;
}