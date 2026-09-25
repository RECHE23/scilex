Python
======

.. module:: scilex

.. autoclass:: scilex.Lexer
   :members:

.. autoclass:: scilex.Token
   :members: kind, lexeme, position, mode, offset, line, column

.. autoclass:: scilex.Position
   :members: offset, line, column

.. autoclass:: scilex.Layout
   :members:

.. autofunction:: scilex.tokenize
.. autofunction:: scilex.scan
.. autofunction:: scilex.layout

Grammars
--------

.. autofunction:: scilex.parse_grammar
.. autofunction:: scilex.load_grammar
.. autoclass:: scilex.Grammar
   :members:

Errors
------

.. autoexception:: scilex.error
.. autoexception:: scilex.LexError
.. autoexception:: scilex.LayoutError
.. autoexception:: scilex.GrammarError

Constants and helpers
---------------------

``END_OF_INPUT``, ``NEWLINE``, ``INDENT``, ``DEDENT`` and ``ERROR`` are the reserved token kinds.

.. autofunction:: scilex.get_include
.. autofunction:: scilex.get_config
.. autofunction:: scilex.real_version
