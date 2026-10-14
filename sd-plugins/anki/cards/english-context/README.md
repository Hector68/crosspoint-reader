# english-context (test deck)

Demonstrates the optional **context** field: a sentence the word appears in,
shown above the word on the front side and separated from it by a thin rule.

Every card uses the full five-field form

```
word<TAB>transcription<TAB>translate<TAB>context[<TAB>image]
```

except `rain`, which has no picture and therefore stops after the context field
(that is the four-text-field shape). Fields are separated by real TABs, the file
is UTF-8 without BOM, and the pictures are 1-bit BMPs copied from the
`english-russian` deck.

See `docs/anki-cards.md` for the format and `docs/anki-cards-prompt.md` for a
prompt that makes an LLM produce a deck like this one.
