This "JSON" parser parses a subset of JSON.

It parses "JSON" with the following features:
* Double quoted string keys ( unquoted keys are not yet allowed )
* Double quoted string values
* Unquoted string keys
* Single quoted string keys ( only in C )
* Inset hashes
* C styles comments ( both //comment and /*comment*/ )
* Escaped double quotes within keys ( only in C )
* Arrays
* N-digit positive and negative integers
* Booleans ( true/false ) ( only in C )
* null ( only in C )

The following JSON features are not supported currently:
* Booleans ( true/false ) ( for Golang )
* Numbers with . or exponents
* Escaped double quotes within values
* null ( for Golang )

The following additional "features" exist:
* Commas between things can be added or left out; they have no effect
* The contents of quoted keys/values are entirely untouched. They are not processed.
* Garbage between things is ignored. For example, single line comments without //
  don't affect parsing so long as they don't contain any double quotes.
* Null characters have no special meaning to the parser and are ignored.
* String values can contain carriage returns
* Key "case" does matter
* When dumping "JSON", quotes are left off "basic keys" ( keys starting with a letter
  and containing only alphanumerics ) - for Golang only currently

Known bugs / deficiencies:
* There is no json__delete yet to clean up a parsed structure when no longer needed
* Functions aren't prefixed with anything like ujson, nor are any kept private, so
  all of them essentially taint your global namespace and could interfere with other
  functions.
* node_hash__get and node_hash__store could be avoided entirely. They've been left
  in because it is cleaner. They should be optimized away automatically during
  compile anyway. Same with node_hash__new and node_str__new for that matter.
* Error is currently never set to anything. There used to be a single error, but it
  wasn't needed and was pointless so I removed it. If you feed the parser garbage,
  you'll just get garbage back. -shrug-
* No malloc / calloc memory allocations are checked to see if they return null. If
  you run out of memory the program will just crash when it tries to use a null
  pointer. -shrug-
* JSON longer than 'int' size ( of your compiler ) isn't currently supported. If you
  are running on a 16-bit platform, this means 32k limit to JSON on those platforms.
  It depends on both your platform and your compiler really.
* If you have a "unquoted value" other than true, false, or null, the parser currently crashes and exits
  intentionally right now. This is obviously not the best result, and will be fixed soon. The reason
  it is not implemented better yet is because doing it as desired is much more complicated due to types.
  ( see the proposal below )
  
Proposols:
* Extensions
  Example format:
  {
    hexdata: x.[hex chars]
    flags: bin.1011
    bindata: b64.[base 64 chars],
    heredoc: hd.<<d undent
        fdfsfd
    d
    data: cdata.
[[ 
    ]],
    raw: raw.len=10.[10 bytes of raw data],
    file: include.file="./somefile",
    mydate: date.2011/12/17,
    mydt: dt.2011/12/13 14:45.1532 PT,
  }
  Method of implementation:
    Use an extension API to register 'b64' as a new data type. A handler is called
    with the buffer and position once that point is reached, and the extension is
    responsible for parsing the data into a structure.
    
    The extension also needs to register the structures it uses, and accept a numerical
    type assigned to those structures.
    
    The extension is responsible for serialization of that type as well when it is
    encountered.
    
    Extensions should register themselves to obtain a namespace. Without such registration
    the file would have to be annotated as well with and identifier of what extension is
    intended to be used by the various extensions.
    
    If multiple extensions are capable of handling the same "format specification" then
    they will be allowed to share the same namespace and the users able to choose which
    extension they want to use for that format.
    
    For file include, the included file should have a whitelisting of the file(s) it can be included into,
    to prevent the parser from being used to gain access to files it should not have access to.