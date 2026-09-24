/*!
 * \file version.hpp
 * \brief SciLex's version, for the preprocessor and at run time.
 *
 * `make release` rewrites the three numeric macros; \ref SCILEX_VERSION_STRING derives from them, and
 * `make version-check` refuses a header that disagrees with pyproject.toml.
 */
#ifndef SCILEX_VERSION_HPP
#define SCILEX_VERSION_HPP

// Preprocessor macros, not constants: a consumer branches on `#if SCILEX_VERSION_MAJOR >= …`, and the
// string is built by stringization, which only the preprocessor does.
// `make release` rewrites these three lines whole, so their documentation must sit above them.
// NOLINTBEGIN(cppcoreguidelines-macro-to-enum,modernize-macro-to-enum,cppcoreguidelines-macro-usage)
/*! \brief Major version (the calendar year). */
#define SCILEX_VERSION_MAJOR 2026
/*! \brief Minor version (the calendar month). */
#define SCILEX_VERSION_MINOR 9
/*! \brief Patch version (the release count within the month). */
#define SCILEX_VERSION_PATCH 1
/*! \brief Inner half of the two-level stringize: turns its argument into a string literal. */
#define SCILEX_STRINGIZE_IMPL(x) #x
/*! \brief Stringizes the *expansion* of \p x — what the second level buys. */
#define SCILEX_STRINGIZE(x)      SCILEX_STRINGIZE_IMPL(x)
/*! \brief The version as "MAJOR.MINOR.PATCH". */
#define SCILEX_VERSION_STRING                          \
        SCILEX_STRINGIZE(SCILEX_VERSION_MAJOR) "."     \
        SCILEX_STRINGIZE(SCILEX_VERSION_MINOR) "."     \
        SCILEX_STRINGIZE(SCILEX_VERSION_PATCH)
// NOLINTEND(cppcoreguidelines-macro-to-enum,modernize-macro-to-enum,cppcoreguidelines-macro-usage)

#endif // SCILEX_VERSION_HPP
