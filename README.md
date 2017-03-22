# `trck`: Query engine for TrailDB

`trck` is a tool to query TrailDBs for aggregate metrics based on individual user behavior.

Typical use cases:
* Calculating behavior-based KPIs (like bounce rates)
* Attributions
* Extracting features from discrete time series data for machine learning

Example queries:
* find number of cases where there is no event of type `page view` within 5 seconds of an event of type `click`
* find all user sessions that "bounced" for campaign X
* find number of user sessions that spend more than 5 minutes on a website

### Table of contents

* [Overview](#overview)
* [Command line tools](#installation)
* [`trck` language](#trck-language-syntax)
* [Implementation details](#implementation-details)
* [Contributors](#contributors)

# Overview

`trck` is a domain specific language that defines a finite state machine<sup>1</sup> to find patterns in data. These programs are compiled into highly optimized parallel native code.

`trck` also includes a number of higher level, data-aware optimizations to make processing as efficient as possible, e.g. by looking ahead at the data and skipping parts that have no chance matching a condition altogether.

### Data Model

The data type `trck` programs deal with is a sequence of timestamped events: a log file or a browsing history is a common example. `trck` uses [TrailDB](https://traildb.io) as a storage engine that is optimized for storing large collections of such histories, in a way that makes it easy to process them one by one.

Of course, one can always use TrailDB API directly to analyze that data; however that is often cumbersome and error prone when you need to match complex patterns in trails. Especially when you need to keep track of current matching state for lots of trails simultaneously, when your data is split across multiple TrailDBs.

### State Machines

`trck` approach is to explicitly define a state machine which is then executed for each trail separately. There is no global state you need to keep track of, you only focus on one trail at a time.

A `trck` program can be considered a device that receives events one by one and reacts to them by transitioning to a different state.

![Compiler pipeline](https://github.com/traildb/trck/blob/master/doc/images/state_machine2.gif)

The output of `trck` program is generated by actions attached to state transitions. For example, you can increment a counter every time state machine transitions from state A to state B.

### Optimizations

State machines are compiled to efficient machine code to ensure maximum performance. On top of that, `trck` compiler applies a few high-level optimizations to skip processing parts of the trails when it has no effect on computation result. With other optimizations, like compressing states to maximize cache efficiency and multicore support, it makes possible for `trck` programs to process millions of trails per second.

See [this slide deck](http://slides.com/oavdeev/deck?token=oZg0_XV0) for some additional details.

###  Testing
Matching trail patterns reliably can be very tricky because of a large number of edge cases; that's why `trck` has a built-in unit test framework and a quickcheck-style property based testing library.


# Installation

### Requirements:

* Ubuntu / Debian Linux / OSX
* Debian packages `bash python (>=2.7) make python-ply jq libjudy-dev (>=1.0.5-5) libjson-c-dev libcmph-dev libc6-dev libtcmalloc-minimal4`
* C compiler, tested with `gcc` and `clang`.

On OSX, you'll need to install a few packages from `brew`:
```
brew install homebrew/boneyard/judy msgpack google-perftools clang-omp
```

You'll also need to install [TrailDB](http://traildb.io/docs/getting_started/). As of time of writing, it is recommended to install TrailDB from source as the `brew` version has a multithreading bug.

### Building

```
git submodule update --init --remote --recursive
make
```

### How to run
Use `trck` command line compiler from `bin/`.

Example:
```
./bin/trck -c myprogram.tr -o matcher-traildb
```

That will compile your program to a binary `matcher-traildb` that will dynamically linked to `libtraildb`. That binary accepts TrailDB paths as positional arguments and prints results in JSON format to stdout. You can also compile a static binary by using `--static` flag (currently Linux only).

You can specify program parameter values using `--params file.json`, JSON file should contain a dictionary specifying values for every parameter. See [Parameters](#parameters) section for more details.

You can specify output format using `--output-format json|msgpack`. Currently only single result mode is supported for msgpack output; that means that you have to use `merged results` mode if you use `foreach` loops (see below).



### Filters

You can also apply filters to traildb to select events a cookies to process. There are two kinds of filters:

* field filters. You can set field filter by passing `--filter` flag to a compiled `trck` program. Filter format TBD. Same filter will be applied to every trail.
* time window filters. You can pass a path to a csv file using `--window-file` flag for a compiled `trck` program. Every line of the file contains 3 comma separated items: `uuid`, `start_timestamp` and `end_timestamp`. For every trail with specified `uuid`, events having timestamp that doesn't satisfy `start_timestamp <= X <= end_timestamp` are ignored. Trails that don't have an entry in the file are ignored entirely.

### Multicore support

`trck` programs are naturally highly parallelizable. Programs are compiled with [OpenMP](http://openmp.org/) automatically, if available.

You can explicitly enable or disable OpenMP support using `--use-openmp` and `--no-openmp` flags to `trck` compiler. You can control the number of OpenMP threads at run time for a compiled `trck` program by setting  `OMP_NUM_THREADS` environment variable, as usual with OpenMP programs, and by default it will use one thread per core.

# `trck` language syntax

## Matching rule specification

Matching rules describe state machines in a text form. State machines naturally consist of states and rules describing transitions between states.

Normally state transitions are triggered by events in the trail; therefore transition rules are boolean conditions that match event fields.

In `trck` you can also trigger transitions on timeouts; they trigger when machine stays in state X for N seconds, without consuming any events.

### Basic matching
The syntax is similar to Erlang/Prolog but with indentation-based block syntax aka [off-side rule](http://en.wikipedia.org/wiki/Off-side_rule). If you are familiar with Erlang, matcher program is essentially a series of tail recursive message receiver loops, where "messages" are events in the trail.

`trck` program consists of one or more blocks. Block has a name and a body, separated by keyword `receive`. Body consists of multiple pattern matching clauses that are evaluated sequentially. Incoming event must match one of the clauses, and the corresponding action is executed.

```erlang
[block_name] ->
    receive
        ... [condition] -> [action] ...
        ... [condition] -> [action] ...
        ...
    after [timeout] -> [action]

```

Example:
```haskell
main ->
  receive
      type = "click", campaign_id = "A"-> yield $clicks
      * -> repeat
```

This program calculates the number of "click" events for a campaign. `receive` block can also be seen as an infinite loop, for every arriving event we have two pattern matching clauses here. Action here is the `yield` statement, that increments a counter variable `$clicks` every time event in a trail matches the condition.

`repeat` action just proceeds to the next event, and matching process starts again from the first pattern matching clause.

Clauses must be exhaustive: if none of them match, you get a runtime error. Normally you'd have catch-all `*->repeat` clause as the last one.


### Clause syntax

Let's look closer at the matching clause syntax.
```haskell
...
    type = "click", campaign_id = "A"-> yield $clicks, skip_dup
...
```
The part before the `->` arrow specifies matching conditions. A comma means boolean AND. There is no OR operator as of now, but you can emulate it by having multiple clauses.

Part after `->` arrow is clause *action*. It may contain `yield` statements to produce results (like counting events). It also has to contain information on where to go next: it can be either `repeat` to continue with the next event in this block, or it can be a block name like `skip_dup` above. In that case, matching will continue with the **next** event in the trail in the specified block. Another special action is `quit` that terminates state machine, in case if you don't want to process any more events in this trail.

### Block duration and `after` example

You can also limit receive block duration by specifying windows using `after` clause. In this example, we'll again count "click" events but will also skip 3 seconds after every click event we encounter, to discard potential duplicates.

```haskell
main ->
    receive
        type = "click", campaign_id = "A"-> yield $clicks, skip_dup
        * -> repeat
skip_dup ->
    receive
        * -> repeat
    after 3s -> main
```

Here, after the first clause in `main` matches, we yield a result, and then go to `skip_dup` block that just loops over until after 3 seconds, then goes back to the `main` block.

`after` clauses cannot use `repeat` as a transition, and naturally there is no matching condition besides time window specification. Time window can be specified as number and time unit (day/hour/minute/second), e.g. `31h`, `3d`, `16m`, `53s`.

### Nested windows

You can nest blocks within a parent `window` block. This is useful when you need to use multiple blocks to implement a matching pattern, but there is "global" expiration period that needs to cover multiple blocks.

The syntax is `window ... after`:
``` haskell
parentblock1 ->
  window
      block1 ->
          ... clauses ...
        block2 ->
          ... clauses ...
    after 30d -> someblock
```

Nested blocks can contain transitions to blocks that are outside the window, but not vice versa.

I.e. this is legal, `block1` can transition to `foo`:
``` haskell
...
foo ->
  ...
parentblock1 ->
  window
        block0 ->
          ... clauses ...
      block1 ->
          type = "X" -> foo
        block2 ->
          ... clauses ...
    after 30d -> someblock
```

But you can't go from `foo` to `block1`, this code won't compile:
``` haskell
foo ->
  receive
      type = "X" -> block1
    * -> repeat

parentblock1 ->
  window
      block0 ->
          ... clauses ...
      block1 ->
          ... clauses ...
        block2 ->
          ... clauses ...
    after 30d -> someblock
```


### Parameters

Jobs may use parameters that are passed externally to them in matching conditions:

```haskell
main ->
  receive
      type = "click", campaign_id = %id -> yield $clicks
      type = "click", campaign_id in #foo -> yield $clicks
      * -> repeat

```

`%id` and `#foo` can be passed at run time to the compiled program. Parameters can be scalars like above, or sets of values. Set parameter names start with `#`, scalar names start with `%`.

You can only compare parameters to field values, not other parameters or constants. That limitation allows the compiler to apply very aggressive optimization techniques. Note that parameters also have (implicit) field "types", e.g. you cannot compare `%id` to `campaign_id` field value in one clause and compare it to `user_id` field value in another. You can work around this by simply having two separate parameters.

### Multiple parameter group matching with `foreach`

For batch processing, you often need to run the program for a number of values; for example, finding number of clicks for every campaign. That, of course, could be done by making campaign id a parameter and running program N times for every parameter value. However, that may be pretty inefficient especially if you have more than a few values.

That's where `foreach` statement is useful. You can create a top level for-loop that loops through values and executes the program for every value, then separate result set is produced for each iteration. That is, in above example, every campaign would get a separate conversion counter. This is the same as if you ran your program N times, once for each campaign, except performance is much better.

Syntax is
```
foreach %bar,#something,%foo in @param
   block1 ->
       receive
           ....
   ...
```

Here `@param` is specified in parameter config file (see `--params` option for compiled binary), and contains a list of tuples, each item type should match the corresponding variable.

In the above example, first and third items of every tuple must be strings and the second item is a set (encoded in JSON as array):
```
{
  "@param" : [
    ["A", ["item1", "item2"], "B"]
    ["Z", ["item1", "item3"], "C"]
  ]
}
```

If you use `quit` action within `foreach` statement, it will only stop trail processing for this specific parameter set (tuple).

One can think of `foreach` as a state machine cloning operation; without `foreach` you have a separate instance of your state machine for each trail, with separate state. With `foreach` and N tuples as parameters, you'll instead have N instances of your state machine for every trail. Therefore `quit` statement only terminates one of these state machines.

#### Implicit `foreach` arrays

Sometimes you just want to run `foreach` over a simple list of all values of some field occurring in the traildb. For example, run your query over all possible campaigns. Since extracting all of them manually and saving to a parameters config file can be a bit tedious -- all required information is already in traildbs anyway -- there is a shortcut for this case:

```
foreach %cid
   block1 ->
       receive
           campaign_id = %cid, ... -> ...
           ....
   ...
```

You can omit array parameter name in the `foreach` clause, in that case, matcher will extract list of possible field values from traildbs and will loop over them.

### Returning results: `yield` statements

`yield` is used to return results from the program. As discussed in the previous section, a separate result is returned for each iteration of `foreach`. Meaning that if you want to run your program over multiple configurations passed in `foreach` array, you'll get a separate result for each configuration.

##### Merged results

If you want `trck` to automatically merge these results, you can add `merged results` qualifier to your `foreach` loop:

```haskell
foreach %aeid,%seid in @arr merged results
 ...
```





#### Counting things: yield to a counter

Simplest form of `yield` is 
```haskell
yield $counter
```

Where `$counter` is an output variable. This statement would increase counter named `$counter` by one. There is no need to declare output variables in advance.

###### Example:
```haskell
foreach %cid in @arr
    start ->
        receive
            type = "cli", campaign_id = %cid -> yield $clicks
            * -> repeat
```
This example would produce a separate counter for each item in `@arr`, e.g.:
```json
[
  {"%cid" : "A", "$clicks" : 5},
  {"%cid" : "B", "$clicks" : 3}
]
```

#### Counting things: yield field to a set

Another form of `yield` allows you to return field values as a tuple. Syntax is
```haskell
yield field1,field2 to #result
```

Where `field1`, `field2` are field names and `#result` is an output variable. `#result` is a set: that is, yielding the same
tuple of fields multiple times will produce only one copy in `#result`. In JSON format as returned by `trck`, variable `#result` would be a list of strings, each string containing field set separated by commas.

#### Counting things pt II: yield field to a multiset

Very similar to sets, can be used in the case when you don't only want distinct items but also their counts. Instead of producing an array of distinct items, you get a dictionary that contains items as keys and counts as values.

```haskell
yield field1,field2 to &result
```

In JSON format as returned by `trck`, variable `&result` would look like this:

```json
...
"&result" : { "foo" : 1, "bar" : 2}
...
```

#### Calling user defined functions in yield

You can call external functions when `yield`ing to a set or a multiset; just drop a file with the same name as your `.tr` script but with `.tr.c` extension and you'll be able to call functions from that file in `yield` statements. See `test_ffi.tr` in `test/tr` directory for an example.

## Complete example:

```haskell
foreach %cid in @arr
    start ->
        receive
            type = "cli", campaign_id = %cid -> yield segment_eid,ad_eid to #ads
            type = "imp", campaign_id = %cid -> yield domain to &domains
            * -> repeat
```

As described above, this example produces a separate result for each item in `@arr`, e.g.:
```json
[
  {"%cid" : "A", "#ads" : ["seg1,ad1", "seg2,ad2"], "&domains" : {"example.com" : 12, "news.org" : 41} },
    {"%cid" : "B", "#ads" : ["seg1,ad2"], "&domains" : {"example.com" : 84, "news.org" : 11}}
]
```



If you used `merged results` mode, you'll get a single result dictionary:

```haskell
foreach %cid in @arr merged results
    start ->
        receive
            type = "cli", campaign_id = %cid -> yield segment_eid,ad_eid to #ads
            type = "imp", campaign_id = %cid -> yield domain to &domains
            * -> repeat
```



Produces following output instead:

```json
{"#ads": ["seg1,ad1", "seg2,ad2", "seg1,ad2"], "&domains": {"example.com": 96, "news.org": 52}}
```



#### Special variables

In addition to returning values of traildb fields in `yield ... to` statements, you can also return special variables:
* `timestamp` contains current event timestamp in the trail
* `cookie` contains current cookie as a hexadecimal string
* `start_timestamp` contains timestamp when current `receive` block was entered. Right now it is only possible to use this variable if that block has an `after` clause, i.e. block cannot be infinite.
* `start_timestamp[label]` contains timestamp when parent `window` block with the name `label` was entered. It is only possible to use this variable within the referred block.

# Implementation details

![Compiler pipeline](https://github.com/traildb/trck/blob/master/doc/images/compiler.png "Compiler pipeline")

A program first parsed and converted to intermediate JSON-based format. `trparser.py` does this and it is fairly independent of the rest of the pipeline.

Second, `fsm2c.py` produces a C module and a header from that JSON representation. That contains state machine itself. That part is technically independent of the underlying storage and can be used with a storage engine other than traildb or even online event processing system that doesn't store entire trails.

Then C module produced by `fsm2c.py` is compiled with `match_traildb.c` which provides necessary primitive functions that are called by generated code to access data from traildbs and also implements efficient `foreach` block execution, setting parameters, collecting results and printing them.


<sup>1</sup> _arguably not a finite state machine in a strict sense, but close enough_

# Contributors

- [Oleg Avdeev](https://github.com/oavdeev)
- [Mikko Juola](https://github.com/Noeda)
- [Vlad Kluev](https://github.com/vladkluev)
- [Ricardo Murillo](https://github.com/rcjmurillo)
- [Knut Nesheim](https://github.com/knutin)
- [Ville Tuulos](https://github.com/tuulos)
