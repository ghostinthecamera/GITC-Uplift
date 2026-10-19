# Contributing translations

Uplift's English and Simplified Chinese interface is maintained in one source catalogue: [src/ui/localization.cpp](src/ui/localization.cpp), in the `CHINESE` table. Translations are compiled into the addons, so there are no language packs or extra runtime files.

## Find translations to update

1. Sync your fork with this repository's `main` branch before editing.
2. Search the catalogue for `TODO(zh-CN)`. From the repository root, run:

   ```sh
   git grep -n -F 'TODO(zh-CN)' -- src/ui/localization.cpp
   ```

3. Each marked entry contains the exact English interface text on the left and an English placeholder on the right:

   ```cpp
   // TODO(zh-CN): translate
   {"New option description", "New option description"},
   ```

4. Replace only the right-hand string with Simplified Chinese, then remove the TODO comment:

   ```cpp
   {"New option description", "新选项说明"},
   ```

Existing translations can also be corrected directly in the right-hand string. No search results means there are no marked pending translations; it does not prove that every interface string has a catalogue entry. If you find English interface text that should be translated, search for its exact wording in the catalogue and add an entry if it is missing.

## Editing rules

- The English string is the lookup key. Keep it identical to the English wording used by the interface, including punctuation and spaces.
- Keep the table sorted by its English keys, in the existing case-sensitive order, with no duplicate keys.
- Preserve formatting placeholders such as `{}`, `{:.1f}`, `%s` and `%u` exactly, in the same order. Preserve intentional `\n` line breaks and paragraph spacing. Values are inserted in their original order; numbered placeholders do not enable reordering.
- Save the source as UTF-8. Preserve C++ string escaping, including `\"` for embedded quotation marks and `\\` for a literal backslash.
- Translate visible wording only. Keep technical identifiers, model names, configuration keys, paths and ImGui `##`/`###` identifiers unchanged. Diagnostic logs keep their canonical wording.
- Pending translations use the English text in both strings plus the TODO comment. Do not use an empty right-hand string: it currently produces blank interface text.

## Check and submit

Follow the [build instructions](README.md#building-from-source), select Chinese in Uplift and inspect the affected controls, tooltips, warnings and readouts. Check for clipped text and confirm that numbers and model names are still correct. Use a Chinese-capable ReShade font as described under [Interface language](README.md#interface-language).

Open a pull request against `main` with the translation changes. For wording that is unclear without context, mention the control or attach a screenshot in the pull request. Maintainers bring accepted translations into the development repository and run its localization checks before exporting a release. Those checks cover catalogue integrity and the translated settings labels, tooltips, choices and enable warnings; other interface text also needs review.

## Process for UI changes

Every change that adds or rewords user-facing interface text must update the catalogue in the same change. This includes labels, choices, tooltips, warnings and status/readout text.

- Supply the Chinese translation when it has been reviewed. Otherwise, add the English text to both fields and mark the entry with `// TODO(zh-CN): translate`.
- When English wording changes, review its Chinese translation too. Until it has been reviewed, replace that translation with the new English text and mark it as pending. Retain an old English entry only if that wording is still used elsewhere.
- Keep placeholders and ordering correct, and check the pending list during review. The marker is a source-maintenance convention, not an automatic string extractor or a missing-translation report.
- Maintainers run the private localization and schema-coverage checks before release. Pending setting translations are reported by the coverage checks and need resolution before those checks pass.
