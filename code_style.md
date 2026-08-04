The style applies to all code you write, in general.
The examples may be in python, but they apply equally to C as much as possible.

# Code style
    1. Use clear yet concise names for symbols. 

        Ideally, code should be self-documenting. This is not possible,
        sometimes - you can use comments, then. It is ok to have long function
        or variable names. The names should be as descriptive as possible and as
        short as possible. For example:

        ```
        for i in things:
        ```
        ^^ will be confusing on the inside of the loop.
        ```
        for inner_sub_thing in things:
        ```
        ^^ totally needlessly verbose.
        ```
        for thing in things:
        ```
        ^^ perfect.

    2. Repeat yourself thrice before abstracting.
        
        Abstracting early makes code harder to maintain, and the abstractions
        are always poorly picked. Abstractions should appear organically - if
        you're doing something similar many times, then you can abstract away.
        Otherwise, it is not useful.

        Even in the case where you're repeating yourself many times, sometimes,
        if each repetition is varied in some way, it is best to just repeat
        yourself with minor variations in code instead of writing a
        mega-function with many arguments that make it so abstract it's
        impossible to follow.

    3. Avoid unnecessarily wrapping code in functions
        
        This is very bad (pseudocode):

        ```
        def to_datetime(time):
            return np.datetime64(time, "1d")
        ```   
        Unless the one-liner is doing something complicated AND it is used MORE
        THAN 3 TIMES IN YOUR CODE, DO NOT WRAP IT IN A FUNCTION CALL. This makes
        it hard to track what is actually going on.

        This is a shooting offence if you perform multiple wraps. Ideally, code
        should be shallow.

    4. Avoid unnecessary classes

        If your class contains a single function or two functions, it is likely
        it shouldn't exist, unless it makes the code particularly elegant.

        Every class incurs both performance and mental overhead. Classes can
        make debugging harder, especially if the functions rely on internal
        class state too much.
        
        You should, whenever possible, write functions with primitive arguments
        that return values and carry no state.

        In fact, if you want to write a new class, then, unless it's a strategy,
        you should ask me for permission explaining why it makes sense to write
        it.

    5. Avoid hidden state, avoid global state

        Our entire code infrastructure is based on having only a single state
        that the ExchangeStateAccumulator (and its cousins) accumulate, and it
        is then basically used to call our strategies almost like functions.

        The strategies should have almost no internal state, apart from
        hyperparameters.

        The reasons for this are detailed more in
        `mandragora/claude_cycle/examples/strategies/strategies.md` .

    6. Avoid installing new packages.

        If you want to install a new package, you must explicitly ask me for
        permission every time. This is especially true for various API wrappers.

        Almost always, the wrappers are a maintenance nightmare. We already have
        REST and websocket capabilities built into python / we might even have
        some specialized packages already installed. It is very easy to just
        perform simple REST requests or manually set up websocket connections.
        Just do it manually.

    7. Avoid using slow packages
        
        Whenever possible, prefer numpy over pandas. Numpy is a lot faster.

        Whenever possible, prefer using no packages over another external
        package. We already suffer from package bloat.

    8. Avoid separating code into multiple files

        Most of the time, one file per strategy or indicator is all you need. In
        fact, multiple related strategies can live in a single file if they are
        generally used as a unit. Don't make me jump around a lot of files to
        understand your logic.

    9. Try to write short code

        Don't do this at the expense of clarity, but, in general, good solutions
        don't have to be long. This does not refer to variable names, but to
        logic.

# Extra code style notes for C:
    10. Always use brackets with if/else blocks, it is not allowed to omit
        them for single-statement if/else blocks.
    11. Because of 10, it is not necessary to wrap macros in do/while loops.
    12. If you ever write a function that allocates memory on the heap, it needs
        to have "alloc" in the name.
    13. All binaries must have the extension .bin .
    14. Prefer STATIC LINKING almost always. If you're linking into something
        that's incredibly large, perhaps we shouldn't even be using it, bring it
        to my attention.

# OTHER
    You are NOT ALLOWED to do any initialization / commiting / pushing of GIT!!!

    you mustn't use spaces in file names - snake_case_is_the_rule.
