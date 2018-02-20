
#include "controlr.hpp"
#include "pipe.hpp"
#include "util.hpp"
#include "controlr_rinterface.h"
#include "variable.pb.h"
#include "message_utilities.h"
#include "console_message.h"

#include "process_exit_codes.h"

#include <stack>

#define MESSAGE_OVERHEAD 64

// pipe index of callback
#define CALLBACK_INDEX          0

// pipe index of primary client
#define PRIMARY_CLIENT_INDEX    1

// #include <google\protobuf\util\json_util.h> // dev

extern BERTBuffers::CallResponse& RCall(BERTBuffers::CallResponse &rsp, const BERTBuffers::CallResponse &call);
extern BERTBuffers::CallResponse& RExec(BERTBuffers::CallResponse &rsp, const BERTBuffers::CallResponse &call);

extern BERTBuffers::CallResponse& ListScriptFunctions(BERTBuffers::CallResponse &rsp, const BERTBuffers::CallResponse &call);

extern bool ReadSourceFile(const std::string &file);

std::string pipename;
std::string rhome;

//HANDLE console_out_event = ::CreateEvent(0, TRUE, FALSE, 0);
int console_client = -1;

std::vector<Pipe*> pipes;
std::vector<HANDLE> handles; //  = { console_out_event };
std::vector<std::string> console_buffer;

// flag indicates we are operating after a break; changes prompt semantics
bool user_break_flag = false;

std::stack<int> active_pipe;


/** debug/util function */
void DumpJSON(const google::protobuf::Message &message, const char *path = 0) {
  std::string str;
  google::protobuf::util::JsonOptions opts;
  opts.add_whitespace = true;
  google::protobuf::util::MessageToJsonString(message, &str, opts);
  if (path) {
    FILE *f;
    fopen_s(&f, path, "w");
    if (f) {
      fwrite(str.c_str(), sizeof(char), str.length(), f);
      fflush(f);
    }
    fclose(f);
  }
  else std::cout << str << std::endl;
}

std::string GetLastErrorAsString(DWORD err = -1)
{
  //Get the error message, if any.
  DWORD errorMessageID = err;
  if (-1 == err) errorMessageID = ::GetLastError();
  if (errorMessageID == 0)
    return std::string(); //No error message has been recorded

  LPSTR messageBuffer = nullptr;
  size_t size = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
    NULL, errorMessageID, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&messageBuffer, 0, NULL);

  std::string message(messageBuffer, size);

  //Free the buffer.
  LocalFree(messageBuffer);

  return message;
}

void DirectCallback(const char *channel, const char *data, bool buffered) {

}

/**
 * frame message and push to console client, or to queue if
 * no console client is connected
 */
void PushConsoleMessage(google::protobuf::Message &message) {

//  static uint32_t cmid = 100000;
//  message.set_id(cmid++);

  std::string framed = MessageUtilities::Frame(message);
  if (console_client >= 0) {
    pipes[console_client]->PushWrite(framed);
  }
  else {
    console_buffer.push_back(framed);
  }
}

void ConsoleResetPrompt(uint32_t id) {
  BERTBuffers::CallResponse message;
  message.set_id(id);
  //message.set_control_message("reset-prompt");
  message.mutable_function_call()->set_target(BERTBuffers::CallTarget::system);
  message.mutable_function_call()->set_function("reset-prompt");
  PushConsoleMessage(message);
}

void ConsoleControlMessage(const std::string &control_message) {
  BERTBuffers::CallResponse message;
  //message.set_control_message(control_message);
  message.mutable_function_call()->set_target(BERTBuffers::CallTarget::system);
  message.mutable_function_call()->set_function(control_message);
  PushConsoleMessage(message);
}

void ConsolePrompt(const char *prompt, uint32_t id) {
  BERTBuffers::CallResponse message;
  message.set_id(id);
  message.mutable_console()->set_prompt(prompt);
  PushConsoleMessage(message);
}

void ConsoleMessage(const char *buf, int len, int flag) {
  BERTBuffers::CallResponse message;
  if (flag) message.mutable_console()->set_err(buf);
  else message.mutable_console()->set_text(buf);
  PushConsoleMessage(message);
}

bool ConsoleCallback(const BERTBuffers::CallResponse &call, BERTBuffers::CallResponse &response) {

  if (console_client < 0) return false; // fail
  Pipe *pipe = pipes[console_client];

  if (!pipe->connected()) return false;

  pipe->PushWrite(MessageUtilities::Frame(call));
  pipe->StartRead(); // probably not necessary

  // we need a blocking write...

  // temp (ugh)
  while (pipe->writing()) {
    pipe->NextWrite();
    Sleep(1);
  }

  std::string data;
  DWORD result;
  do {
    result = pipe->Read(data, true);
  } 
  while (result == ERROR_MORE_DATA);

  if (!result) MessageUtilities::Unframe(response, data);

  pipe->StartRead(); // probably not necessary either
  return (result == 0);

  return false;

}

bool Callback(const BERTBuffers::CallResponse &call, BERTBuffers::CallResponse &response) {

  Pipe *pipe = 0;

  if (active_pipe.size()) {
    int index = active_pipe.top();
    pipe = pipes[index];

    // dev
    if (index != 1) cout << "WARN: callback, top of pipe is " << index << endl;

    // don't allow that?
    cout << "(switching to callback pipe)" << endl;
    pipe = pipes[CALLBACK_INDEX];

  }
  else {
    cout << "using callback pipe" << endl;
    pipe = pipes[CALLBACK_INDEX];
  }

  if (!pipe->connected()) return false;

  pipe->PushWrite(MessageUtilities::Frame(call));
  pipe->StartRead(); // probably not necessary

  std::string data;
  DWORD result;
  do {
    result = pipe->Read(data, true);
  } while (result == ERROR_MORE_DATA);

  if (!result) MessageUtilities::Unframe(response, data);

  pipe->StartRead(); // probably not necessary either
  return (result == 0);
}

void Shutdown(int exit_code) {
  ExitProcess(0);
}

void NextPipeInstance(bool block, std::string &name) {
  Pipe *pipe = new Pipe;
  int rslt = pipe->Start(name, block);
  handles.push_back(pipe->wait_handle_read());
  handles.push_back(pipe->wait_handle_write());
  pipes.push_back(pipe);
}

void CloseClient(int index) {

  // shutdown if primary client breaks connection
  if (index == PRIMARY_CLIENT_INDEX) Shutdown(-1);

  // callback shouldn't close either
  else if (index == CALLBACK_INDEX) {
    cerr << "callback pipe closed" << endl;
    // Shutdown(-1);
  }

  // otherwise clean up, and watch out for console
  else {
    pipes[index]->Reset();
    if (index == console_client) {
      console_client = -1;
    }
  }

}

void QueueConsoleWrites() {
  pipes[console_client]->QueueWrites(console_buffer);
  console_buffer.clear();
}

/**
 * in an effort to make the core language agnostic, all actual functions are moved
 * here. this should cover things like initialization and setting the COM pointers.
 *
 * the caller uses symbolic constants that call these functions in the appropriate
 * language.
 */
bool SystemCall(BERTBuffers::CallResponse &response, const BERTBuffers::CallResponse &call, int pipe_index) {
  std::string function = call.function_call().function();

  BERTBuffers::CallResponse translated_call;
  translated_call.CopyFrom(call);

  if (!function.compare("install-application-pointer")) {
    translated_call.mutable_function_call()->set_target(BERTBuffers::CallTarget::language);
    translated_call.mutable_function_call()->set_function("BERT$install.application.pointer");
    RCall(response, translated_call);
  }
  else if (!function.compare("list-functions")) {
    //translated_call.mutable_function_call()->set_target(BERTBuffers::CallTarget::language);
    //translated_call.mutable_function_call()->set_function("BERT$list.functions");
    //RCall(response, translated_call);
    ListScriptFunctions(response, call);
  }
  else if (!function.compare("get-language")) {
    response.mutable_result()->set_str("R");
  }
  else if (!function.compare("read-source-file")) {
    std::string file = call.function_call().arguments(0).str();
    bool success = false;
    if( file.length()){
      std::cout << "read source: " << file << std::endl;
      success = ReadSourceFile(file);
    }
    response.mutable_result()->set_boolean(success);
  }

  ///
  else if (!function.compare("shutdown")) {
    ConsoleControlMessage("shutdown");
    Shutdown(0);
  }
  else if (!function.compare("console")) {
    if (console_client < 0) {
      console_client = pipe_index;
      cout << "set console client -> " << pipe_index << endl;
      //pipe->QueueWrites(console_buffer);
      //console_buffer.clear();
      QueueConsoleWrites();
    }
    else cerr << "console client already set" << endl;
  }
  else if (!function.compare("close")) {
    CloseClient(pipe_index);
    return false; //  break; // no response 
  }
  else {
    std::cout << "ENOTIMPL (system): " << function << std::endl;
    response.mutable_result()->set_boolean(false);
  }

  return true;

}

int InputStreamRead(const char *prompt, char *buf, int len, int addtohistory, bool is_continuation) {

  // it turns out this function can get called recursively. we
  // hijack this function to run non-interactive R calls, but if
  // one of those calls wants a shell interface (such as a debug
  // browser, it will call into this function again). this gets
  // a little hard to track on the UI side, as we have extra prompts
  // from the internal calls, but we don't know when those are 
  // finished.

  // however we should be able to figure this out just by tracking
  // recursion. note that this is never threaded.

  static uint32_t call_depth = 0;
  static bool recursive_calls = false;

  static uint32_t prompt_transaction_id = 0;

  std::string buffer;
  std::string message;

  DWORD result;

  if (call_depth > 0) {
    // set flag to indicate we'll need to "unwind" the console
    cout << "console prompt at depth " << call_depth << endl;
    recursive_calls = true;
  }

  ConsolePrompt(prompt, prompt_transaction_id);

  while (true) {

    result = WaitForMultipleObjects((DWORD)handles.size(), &(handles[0]), FALSE, 100);

    if (result >= WAIT_OBJECT_0 && result - WAIT_OBJECT_0 < 16) {

      int offset = (result - WAIT_OBJECT_0);
      int index = offset / 2;
      bool write = offset % 2;
      auto pipe = pipes[index];

      if (!index) cout << "pipe event on index 0 (" << (write ? "write" : "read") << ")" << endl;

      ResetEvent(handles[result - WAIT_OBJECT_0]);

      if (!pipe->connected()) {
        cout << "connect (" << index << ")" << endl;
        pipe->Connect(); // this will start reading
        if (pipes.size() < MAX_PIPE_COUNT) NextPipeInstance(false, pipename);
      }
      else if (write) {
        pipe->NextWrite();
      }
      else {
        result = pipe->Read(message);

        if (!result) {

          BERTBuffers::CallResponse call, response;
          bool success = MessageUtilities::Unframe(call, message);

          if (success) {

            response.set_id(call.id());
            switch (call.operation_case()) {

            case BERTBuffers::CallResponse::kFunctionCall:
              call_depth++;
              active_pipe.push(index);
              switch (call.function_call().target()) {
              case BERTBuffers::CallTarget::system:
                SystemCall(response, call, index);
                break;
              default:
                RCall(response, call);
                break;
              }
              active_pipe.pop();
              call_depth--;
              if (call.wait()) pipe->PushWrite(MessageUtilities::Frame(response));
              break;

            case BERTBuffers::CallResponse::kCode:
              call_depth++;
              active_pipe.push(index);
              RExec(response, call);
              active_pipe.pop();
              call_depth--;
              if (call.wait()) pipe->PushWrite(MessageUtilities::Frame(response));
              break;

            case BERTBuffers::CallResponse::kShellCommand:
              len = min(len - 2, (int)call.shell_command().length());
              strcpy_s(buf, len + 1, call.shell_command().c_str());
              buf[len++] = '\n';
              buf[len++] = 0;
              prompt_transaction_id = call.id();
              pipe->StartRead();

              // start read and then exit this function; that will cycle the R REPL loop.
                            // the (implicit/explicit) response from this command is going to be the next 
                            // prompt.

              return len;
            /*
            case BERTBuffers::CallResponse::kControlMessage:
            {
              std::string command = call.control_message();
              cout << "system command: " << command << endl;
              if (!command.compare("shutdown")) {
                ConsoleControlMessage("shutdown");
                Shutdown(0);
              }
              else if (!command.compare("console")) {
                if (console_client < 0) {
                  console_client = index;
                  cout << "set console client -> " << index << endl;
                  pipe->QueueWrites(console_buffer);
                  console_buffer.clear();
                }
                else cerr << "console client already set" << endl;
              }
              else if (!command.compare("close")) {
                CloseClient(index);
                break; // no response 
              }
              else {
                // ...
              }

              if (call.wait()) {
                response.set_id(call.id());
                //pipe->PushWrite(rsp.SerializeAsString());
                pipe->PushWrite(MessageUtilities::Frame(response));
              }
              else pipe->NextWrite();
            }
            break;
            */

            default:
              // ...
              0;
            }

            if (call_depth == 0 && recursive_calls) {
              cout << "unwind recursive prompt stack" << endl;
              recursive_calls = false;
              ConsoleResetPrompt(prompt_transaction_id);
            }

          }
          else {
            if (pipe->error()) {
              cout << "ERR in system pipe: " << result << endl;
            }
            else cerr << "error parsing packet: " << result << endl;
          }
          if (pipe->connected() && !pipe->reading()) {
            pipe->StartRead();
          }
        }
        else {
          if (result == ERROR_BROKEN_PIPE) {
            cerr << "broken pipe (" << index << ")" << endl;
            CloseClient(index);
          }
          //else if (result == ERROR_MORE_DATA) {
          //    cout << "(more data...)" << endl;
          //}
        }
      }
    }
    else if (result == WAIT_TIMEOUT) {
      RTick();
    }
    else {
      cerr << "ERR " << result << ": " << GetLastErrorAsString(result) << endl;
      break;
    }
  }

  return 0;
}

unsigned __stdcall ManagementThreadFunction(void *data) {

  DWORD result;
  Pipe pipe;
  char *name = reinterpret_cast<char*>(data);

  cout << "start management pipe on " << name << endl;

  int rslt = pipe.Start(name, false);
  std::string message;

  while (true) {
    result = WaitForSingleObject(pipe.wait_handle_read(), 1000);
    if (result == WAIT_OBJECT_0) {
      ResetEvent(pipe.wait_handle_read());
      if (!pipe.connected()) {
        cout << "connect management pipe" << endl;
        pipe.Connect(); // this will start reading
      }
      else {
        result = pipe.Read(message);
        if (!result) {
          BERTBuffers::CallResponse call;
          bool success = MessageUtilities::Unframe(call, message);
          if (success) {
            //std::string command = call.control_message();
            std::string command = call.function_call().function();
            if (command.length()) {
              if (!command.compare("break")) {
                user_break_flag = true;
                RSetUserBreak();
              }
              else {
                cerr << "unexpected system command (management pipe): " << command << endl;
              }
            }
          }
          else {
            cerr << "error parsing management message" << endl;
          }
          pipe.StartRead();
        }
        else {
          if (result == ERROR_BROKEN_PIPE) {
            cerr << "broken pipe in management thread" << endl;
            pipe.Reset();
          }
        }
      }
    }
    else if (result != WAIT_TIMEOUT) {
      cerr << "error in management thread: " << GetLastError() << endl;
      pipe.Reset();
      break;
    }
  }
  return 0;
}

/*
BOOL WINAPI ConsoleHandler(DWORD signal) {
    if (signal == CTRL_C_EVENT) {
        cout << "ctrl+C" << endl;
    }
    else {
        cout << "other signal " << signal << endl;
    }
    return TRUE;
}
*/



int main(int argc, char** argv) {

  int major, minor, patch;
  RGetVersion(&major, &minor, &patch);

  std::cout << "R version: " << major << ", " << minor << ", " << patch << std::endl;

  if (major != 3) return PROCESS_ERROR_UNSUPPORTED_VERSION;
  if( minor != 4) return PROCESS_ERROR_UNSUPPORTED_VERSION;

  for (int i = 0; i < argc; i++) {
    if (!strncmp(argv[i], "-p", 2) && i < argc - 1) {
      pipename = argv[++i];
    }
    else if (!strncmp(argv[i], "-r", 2) && i < argc - 1) {
      rhome = argv[++i];
    }
  }

  if (!pipename.length()) {
    cerr << "call with -p pipename" << endl;
    return PROCESS_ERROR_CONFIGURATION_ERROR;
  }
  if (!rhome.length()) {
    cerr << "call with -r rhome" << endl;
    return PROCESS_ERROR_CONFIGURATION_ERROR;
  }

  cout << "pipe: " << pipename << endl;
  cout << "pid: " << _getpid() << endl;

  // we need a non-const block for the thread function. 
  // it just gets used once, and immediately

  char buffer[MAX_PATH];
  sprintf_s(buffer, "%s-M", pipename.c_str());
  uintptr_t thread_handle = _beginthreadex(0, 0, ManagementThreadFunction, buffer, 0, 0);

  // start the callback pipe first. doesn't block.

  std::string callback_pipe_name = pipename;
  callback_pipe_name += "-CB";
  NextPipeInstance(false, callback_pipe_name);

  // the first connection blocks. we don't start R until there's a client.

  NextPipeInstance(true, pipename);

  // start next pipe listener, don't block

  NextPipeInstance(false, pipename);

  // now start R 

  char no_save[] = "--no-save";
  char no_restore[] = "--no-restore";
  char* args[] = { argv[0], no_save, no_restore };

  int result = RLoop(rhome.c_str(), "", 3, args);
  if (result) cerr << "R loop failed: " << result << endl;

  handles.clear();
  for (auto pipe : pipes) delete pipe;

  pipes.clear();

  return 0;
}
