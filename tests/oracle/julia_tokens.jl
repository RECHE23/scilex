# Julia's own tokens for each file named on the command line, as julia_juliasyntax.py reads them: a line
# `FILE<TAB>path`, then one `kind<TAB>first byte<TAB>last byte` per token (1-based, inclusive), from the
# tokenizer of the parser Julia ships (Base.JuliaSyntax).
const JS = Base.JuliaSyntax
for path in ARGS
    println("FILE\t", path)
    source = read(path, String)
    for token in JS.tokenize(source)
        println(JS.kind(token), "\t", first(token.range), "\t", last(token.range))
    end
end
