C++
===

``#include <scilex/scilex.hpp>`` brings in the lexer, rules and tokens; ``scilex/layout.hpp`` and
``scilex/grammar.hpp`` are opt-in.

The lexer
---------

.. doxygenclass:: scilex::lexer
   :members: lexer, tokenize, scan, columns, dfa_modes_active, pike_rules, end_of, mode_significant, mode_name

.. doxygenenum:: scilex::eof_policy
.. doxygenenum:: scilex::error_policy
.. doxygenenum:: scilex::column_unit
.. doxygenenum:: scilex::dfa_policy
.. doxygenvariable:: scilex::max_mode_depth

Rules and modes
---------------

.. doxygenstruct:: scilex::rule
   :members:

.. doxygenstruct:: scilex::mode_action
   :members:

Tokens and positions
--------------------

.. doxygenstruct:: scilex::token
   :members:

.. doxygenstruct:: scilex::position
   :members:

.. doxygenclass:: scilex::lex_error
   :members:

Layout (``scilex/layout.hpp``)
------------------------------

.. doxygenfunction:: scilex::layout(std::span<const token>, const std::vector<bool>&)
.. doxygenfunction:: scilex::layout(std::span<const token>, std::string_view, tab_policy, const std::vector<bool>&)
.. doxygenenum:: scilex::tab_policy
.. doxygenclass:: scilex::layout_error
   :members:

Grammars (``scilex/grammar.hpp``)
---------------------------------

.. doxygenfunction:: scilex::parse_grammar
.. doxygenfunction:: scilex::load_grammar
.. doxygenstruct:: scilex::grammar
   :members:
.. doxygenclass:: scilex::grammar_error
   :members:
