// Inputs only: expectations are OBSERVED from the actual pinned Bun at execution.
// A Worker mismatch is a failing source-parity gate, never an ISO/parser fallback.
export const passportDateInputs = Object.freeze([
  ["iso", "2026-10-27T00:00:00.000Z"],
  ["rfc", "Tue, 27 Oct 2026 00:00:00 GMT"],
  ["us_slashes", "10/27/2026"],
  ["month_name", "October 27, 2026"],
  ["year_zero", "0000"],
  ["numeric_legacy", "1"],
  ["date_only", "2026-10-27"],
  ["no_timezone", "2026-10-27T00:00:00"],
  ["date_overflow", "2026-02-29"],
  ["empty", ""],
  ["whitespace", " "],
  ["invalid", "not a date"],
  ["lone_high", "\ud800"],
  ["lone_low", "\udfff"],
  ["nul", "\u0000"],
  ["date_then_nul", "2026-10-27\u0000"],
  ["date_then_lone", "2026-10-27\ud800"],
  ["source_length_100", "2026-10-27" + " ".repeat(90)],
  ["source_length_101", "2026-10-27" + " ".repeat(91)],
]);
export const observePassportDates = () => passportDateInputs.map(([name, value]) => ({
  name, value, units: Array.from({ length: value.length }, (_, index) => value.charCodeAt(index)),
  accepted: value.length > 0 && value.length <= 100 && Number.isFinite(Date.parse(value)),
}));
