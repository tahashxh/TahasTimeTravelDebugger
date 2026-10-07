// ======================= TIME-TRAVEL DEBUGGER - SERVER TEMPLATE =======================

// Pipeline this file implements, top to bottom:
//   0. Receive  -- stream the client's .trace bytes straight to source.bin on disk
//   1. Pass 0X0   -- validity check (FUNC/FUNC_END matching)
//   2. Pass 0X1   -- resolve(): copy EVERY source line into resolve.bin as [offset][size][string], then patch CALL targets.
//   3. Pass 0X2   -- execute resolve.bin: tokenize ONE line at a time, update the call stack, take a snapshot -> Timeline
//   4. Pass 0X3   -- serialize Timeline -> session.tdbg(header + snapshot records + dense index)


#include <iostream>
#include <string>
#include <cstdint>
#include <fstream>
#include <unistd.h>
#include <sys/socket.h>
#include <cstdint>
#include <cstdio>
#include <sstream>
using namespace std;

// ---- Constants ----
const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int32_t MAX_FUNCS = 128;
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2; // kW + func_name + upto 16 params/args
const int32_t MAX_PATCHES = MAX_FUNCS * 4;
const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024; // sanity cap on the declared file length
const int32_t IO_BUFFER_SIZE = 64 * 1024;                  // fixed buffer for streaming to/from disk
const int32_t SOCKET_TIMEOUT_SEC = 5;                      // TODO: apply as SO_RCVTIMEO so a deadclient can't hang the server forever

// ---- Custom data structures

// Stack: back the live Call Stack during execution
template <typename T>
class Stack
{
    struct Node
    {
        T data;
        Node *next;
    };
    Node *top;
    int32_t count;

public:
    // Implement these functions:
    Stack()
    { 
        top = nullptr;
        count = 0;
    }
    void push(const T &val)
    {
       if (count >= MAX_STACK_DEPTH) 
        {
            return; 
        }

        Node *newNode = new Node{val, top};
        top = newNode;
        count++;
    }

    T pop()
    {
        if (isEmpty()) 
        {
            throw std::runtime_error("Stack underflow"); 
        }
        Node *temp = top;
        T poppedData = temp->data;
        top = top->next;
        delete temp;
        count--;
        return poppedData;
    }
    T &peek()
    {
        if (isEmpty()) 
        {
            throw std::runtime_error("Stack is empty"); 
        }
        return top->data;
    }
    bool isEmpty()
    {
        return count == 0;
    }
    int32_t depth()
    {
        return count;
    }
    int32_t snapshot_into(T out[], int32_t maxLen)
    {
        // copies every frame, top to bottom in the array given as a parameter
        // this is what buildSnapshot() call, returns count written
        int32_t written = 0;
        Node *current = top;
        
        while (current != nullptr && written < maxLen) 
        {
            out[written] = current->data;
            current = current->next;
            written++;
        }

        return written;
    }
};


// Timeline : doubly linked list of Snapshots
struct Snapshot; // fwd declaration;
struct TimelineNode
{
    Snapshot *data;
    TimelineNode *next;
    TimelineNode *prev;
};
class Timeline
{
    TimelineNode *head, *tail;
    int32_t stepCount;

public:
    // Implement these functions
    Timeline()
    {
        head = nullptr;
        tail = nullptr;
        stepCount = 0;
    }
    void record(Snapshot *s)
    {
        TimelineNode *newNode = new TimelineNode{s, nullptr, tail};
        if (tail != nullptr) 
        {
            tail->next = newNode;
        } 
        else 
        {
            head = newNode; 
        }
        tail = newNode;
        stepCount++;
    }
    TimelineNode *begin()
    {
        return head;    
    }
    int32_t getStepCount()
    {
        return stepCount;
    }
};

// Core structs
struct Variable
{
    string name;
    int32_t value;
};
struct Frame
{
    string func_name;
    int32_t argc;
    Variable argv[MAX_VARS_PER_FRAME];
    int32_t returnLine;
    Variable locals[MAX_VARS_PER_FRAME];
    int32_t localCount;
};
struct Snapshot
{
    Frame callStack[MAX_STACK_DEPTH];
    int32_t stackDepth;
};
struct TTDBHeader
{
    char magic[4]; // "TTDB"
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};
void writeHeader(FILE *f, const TTDBHeader &h)
{
    fwrite(h.magic, 1, 4, f);
    fwrite(&h.version, sizeof(int32_t), 1, f);

    // placeholder for other two data members
}

// resolve.bin - bookkeeping
struct FuncEntry
{
    string funcName;
    int64_t byteOffsetInResolveBin; // where this function's FUNC header record sits
};
struct PendingPatch
{
    int64_t byteOffsetOfOffsetField; // where in resolve.bin to seek back and overwrite
    string targetFuncName;
};



// PASS 0x0: READING source.bin + VALIDITY CHECK
bool readSourceLine(ifstream &in, string &out)
{
   while (getline(in, out)) 
   {
        bool isBlank = true;
        for (int i = 0; i < out.length(); i++) 
        {
            if (out[i] != ' ' && out[i] != '\t' && out[i] != '\r' && out[i] != '\n') 
            {
                isBlank = false;
                break;
            }
        }
        if (!isBlank) 
        {
            return true; 
        }
    }

    return false;
}
string firstWord(const string &line)
{
    std::istringstream iss(line);
    string word;
    iss >> word; 
    return word;
}
string secondWord(const string &line)
{
    std::istringstream iss(line);
    string word1, word2;
    if (iss >> word1 >> word2) 
    {
        return word2;
    }
    return "";
}
bool validateProgram(const char *fileName)
{
    ifstream in(fileName);
    if (!in.is_open()) 
    {
        return false; 
    }

    Stack<string> scopeStack; 
    string line;

    while (readSourceLine(in, line)) 
    {
        string fw = firstWord(line);
        
        if (fw == "func") 
        {
            if (!scopeStack.isEmpty()) 
            {
                return false; 
            }
            scopeStack.push("func");
        } 
        else if (fw == "func_end") 
        {
            if (scopeStack.isEmpty()) 
            {
                return false; 
            }
            scopeStack.pop();
        }   
    }
    in.close();
    return scopeStack.isEmpty();
}

// PASS 0x1: RESOLVE() -> resolve.bin
int64_t writeResolveRecord(FILE *f, int64_t offsetField, const string &text)
{
    int64_t startPos = ftell(f);
    
    fwrite(&offsetField, sizeof(int64_t), 1, f);
    
    int32_t size = text.length();
    fwrite(&size, sizeof(int32_t), 1, f);
    
    fwrite(text.c_str(), 1, size, f);
    
    return startPos;
}
int64_t readResolveRecord(FILE *f, string &outText)
{
    int64_t offsetField = 0;
    int32_t size = 0;
    
    if (fread(&offsetField, sizeof(int64_t), 1, f) != 1) return -1; 
    
    fread(&size, sizeof(int32_t), 1, f);
    
    outText.resize(size);
    fread(&outText[0], 1, size, f);
    
    return offsetField;
}
int64_t resolveProgram(const char *sourcePath, const char *resolveBinPath)
{
    FuncEntry funcArray[MAX_FUNCS];
    int32_t funcCount = 0;
    PendingPatch patches[MAX_PATCHES];
    int32_t patchCount = 0;

    ifstream in(sourcePath);
    if (!in.is_open()) return -1;

    FILE *out = fopen(resolveBinPath, "wb+");
    if (!out) 
    {
        in.close();
        return -1;
    }

    string line;
    int64_t currentOffset = 0; 

    while (readSourceLine(in, line)) 
    {

        string fw = firstWord(line);
        string sw = secondWord(line);

        int64_t recordFilePos = writeResolveRecord(out, currentOffset, line);

        if (fw == "func") 
        {
            funcArray[funcCount].funcName = sw;
            funcArray[funcCount].byteOffsetInResolveBin = currentOffset;
            funcCount++;
        } 
        else if (fw == "call") 
        {
            patches[patchCount].byteOffsetOfOffsetField = recordFilePos; 
            patches[patchCount].targetFuncName = sw;
            patchCount++;
        }
        
        currentOffset = currentOffset + 8 + 4 + line.length();
    }
    in.close();

    for (int i = 0; i < patchCount; i++) 
    {
        int64_t targetOffset = -1;
        
        for (int j = 0; j < funcCount; j++) 
        {
            if (funcArray[j].funcName == patches[i].targetFuncName) 
            {
                targetOffset = funcArray[j].byteOffsetInResolveBin;
                break;
            }
        }

        if (targetOffset == -1) 
        {
            fclose(out);
            return -1; 
        }

        fseek(out, patches[i].byteOffsetOfOffsetField, SEEK_SET);
        fwrite(&targetOffset, sizeof(int64_t), 1, out);
    }

    int64_t mainOffset = -1;
    for (int i = 0; i < funcCount; i++) 
    {
        if (funcArray[i].funcName == "main") 
        {
            mainOffset = funcArray[i].byteOffsetInResolveBin;
            break;
        }
    }

    fclose(out);
    return mainOffset;
}

// PASS 0x2: EXECUTION (tokenization happens here)
enum TokenType
{
    KEYWORD,
    IDENTIFIER,
    PARAM
};
struct Token
{
    TokenType type;
    string text;
};

int32_t tokenizeLine(const string &line, Token tokens[], int32_t maxTokens)
{
    std::istringstream iss(line);
    string word;
    int32_t count = 0;

    while (iss >> word && count < maxTokens) 
    {
        tokens[count].text = word;
        if (count == 0) 
            tokens[count].type = KEYWORD;
        else if (count == 1) 
            tokens[count].type = IDENTIFIER;
        else 
            tokens[count].type = PARAM;
        
        count++;
    }
    return count; 
}

Snapshot *buildSnapshot(Stack<Frame> &callStack)
{
    Snapshot *snap = new Snapshot();
    snap->stackDepth = callStack.snapshot_into(snap->callStack, MAX_STACK_DEPTH);
    return snap;
}

// helper function
int32_t getVarValue(Frame &frame, const string &text)
{
    if (isdigit(text[0]) || (text[0] == '-' && text.length() > 1)) 
    {
        return stoi(text);
    }
    for (int i = 0; i < frame.localCount; i++) 
    {
        if (frame.locals[i].name == text) return frame.locals[i].value;
    }

    for (int i = 0; i < frame.argc; i++) 
    {
        if (frame.argv[i].name == text) return frame.argv[i].value;
    }


    return 0;
}

// Helper function
void setVarValue(Frame &frame, const string &text, int32_t val)
{
    for (int i = 0; i < frame.localCount; i++) 
    {
        if (frame.locals[i].name == text) 
        {
            frame.locals[i].value = val;
            return;


        }
    }

    for (int i = 0; i < frame.argc; i++) 
    {
        if (frame.argv[i].name == text) 
        {
            frame.argv[i].value = val;
            return;
        }

    }
    if (frame.localCount < MAX_VARS_PER_FRAME) 
    {
        frame.locals[frame.localCount].name = text;

        frame.locals[frame.localCount].value = val;
        frame.localCount++;
    }

}

void executeProgram(const char *resolveBinPath, int64_t mainOffset, Timeline &timeline)
{

    FILE *f = fopen(resolveBinPath, "rb");
    if (!f) return;

    Stack<Frame> callStack;
    fseek(f, mainOffset, SEEK_SET);

    string lineText;
    Token tokens[MAX_TOKENS];

    while (true) 
    {
        int64_t targetOffset = readResolveRecord(f, lineText);
        if (targetOffset == -1) 
            break; 

        int32_t tokenCount = tokenizeLine(lineText, tokens, MAX_TOKENS);

        if (tokenCount == 0) 
            continue;

        string kw = tokens[0].text;

        if (kw == "func") 
        {

            if (callStack.isEmpty()) 
            {
                Frame m;
                m.func_name = tokens[1].text;
                m.argc = 0;
                m.localCount = 0;
                m.returnLine = -1;
                callStack.push(m);

            } 
            else 
            {
                Frame &top = callStack.peek();
                top.func_name = tokens[1].text;
                for (int i = 0; i < top.argc; i++) 
                {
                    top.argv[i].name = tokens[2 + i].text;

                }

            }
        }
        else if (kw == "set") 
        {
            Frame &top = callStack.peek();
            string varName = tokens[1].text;
            int32_t val = getVarValue(top, tokens[2].text);
            setVarValue(top, varName, val);

        }
        else if (kw == "add" || kw == "sub" || kw == "mul" || kw == "div") 
        {
            Frame &top = callStack.peek();

        
            string dest = tokens[1].text; 
            
            int32_t val1 = getVarValue(top, dest);

            int32_t val2 = getVarValue(top, tokens[2].text);
            
            int32_t result = 0;
            if (kw == "add") 
                result = val1 + val2;
            if (kw == "sub") 
                result = val1 - val2;
            if (kw == "mul") 
                result = val1 * val2;
            if (kw == "div") 
                result = val2 != 0 ? (val1 / val2) : 0;
            
            setVarValue(top, dest, result);

        }
        else if (kw == "call") 
        {
            Frame &caller = callStack.peek();

            Frame callee;
            
            callee.returnLine = ftell(f); 
            callee.localCount = 0;
            callee.argc = tokenCount - 2; 
            
            for (int i = 0; i < callee.argc; i++) 
            {
                callee.argv[i].value = getVarValue(caller, tokens[2 + i].text);
            }
            
            callStack.push(callee);
            fseek(f, targetOffset, SEEK_SET); 

        }
        else if (kw == "func_end") 
        {
            Frame finished = callStack.pop();
            
            if (callStack.isEmpty()) 
            {
                timeline.record(buildSnapshot(callStack));
                break;

            }
            else 
            {
                fseek(f, finished.returnLine, SEEK_SET);

            }
            
        }

        timeline.record(buildSnapshot(callStack));
    }

    fclose(f);
}

// PASS 0x3: SERIALIZE TIMELINE
void writeTdbg(Timeline &timeline, const char *tdbgPath)
{
    // placeholder for header
    // index array of the size of stepcount from the timeline
    // placing each snapshot in the file while maintaining the index(starting point of each nth snapshot)
    // after timeline add the index array i the file
    // update the header
}
// main section
int32_t main()
{

    if (!validateProgram("source.bin"))
    {
        // send an error response instead of a .tdbg file
        return 1;
    }

    int64_t mainOffset = resolveProgram("source.bin", "resolve.bin");

    Timeline timeline;
    executeProgram("resolve.bin", mainOffset, timeline);

    writeTdbg(timeline, "session.tdbg");

    return 0;
}
