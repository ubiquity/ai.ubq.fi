export const REASONING_NONE_VALUE = "none";

const getTrimmedString = (value) => (typeof value === "string" ? value.trim() : "");
const getReasoningEffortValue = (value) => value === null ? REASONING_NONE_VALUE : getTrimmedString(value);

const getReasoningLevelValue = (level) => {
  if (level === null || typeof level === "string") return getReasoningEffortValue(level);
  if (level && typeof level === "object" && "effort" in level) {
    return getReasoningEffortValue(level.effort);
  }
  return "";
};

const isNoneReasoningValue = (value) => getTrimmedString(value) === REASONING_NONE_VALUE;
const getRawReasoningLevels = (model) =>
  Array.isArray(model?.supported_reasoning_levels) ? model.supported_reasoning_levels : [];
const hasNoneReasoningLevel = (levels) =>
  Array.isArray(levels) &&
  levels.some((level) => isNoneReasoningValue(getReasoningLevelValue(level)));

const uniqueReasoningLevels = (levels) => {
  const unique = [];
  const seen = new Set();
  levels.forEach((level) => {
    const value = getReasoningLevelValue(level);
    if (!value || isNoneReasoningValue(value)) return;
    if (seen.has(value)) return;
    seen.add(value);
    unique.push(value);
  });
  return unique;
};

export const getModelDefaultReasoning = (model) => {
  const defaultReasoningEffort = getReasoningEffortValue(model?.default_reasoning_effort);
  if (defaultReasoningEffort) return defaultReasoningEffort;
  return getReasoningEffortValue(model?.default_reasoning_level);
};

const modelSupportsNoneReasoning = (model) =>
  hasNoneReasoningLevel(getRawReasoningLevels(model)) || isNoneReasoningValue(getModelDefaultReasoning(model));

export const getModelReasoningLevels = (model) => {
  const rawLevels = getRawReasoningLevels(model);
  const levels = uniqueReasoningLevels(rawLevels);
  if (levels.length) return levels;

  const fallback = getModelDefaultReasoning(model);
  return fallback && !isNoneReasoningValue(fallback) ? [fallback] : [];
};

export const isReasoningNoneSelection = (value, noneValue = REASONING_NONE_VALUE) => {
  const trimmed = getTrimmedString(value);
  if (!trimmed) return false;
  return trimmed === noneValue || isNoneReasoningValue(trimmed);
};

const appendOption = (select, value, label, options = {}) => {
  const option = document.createElement("option");
  option.value = value;
  option.textContent = label;
  if (options.disabled) option.disabled = true;
  select.appendChild(option);
  return option;
};

export const setReasoningPlaceholder = (select, label) => {
  select.textContent = "";
  appendOption(select, "", label, { disabled: true });
  select.disabled = true;
  return "";
};

export const setReasoningOptions = (select, levels, preferred, options = {}) => {
  const sourceLevels = Array.isArray(levels) ? levels : [];
  const noneValue = getTrimmedString(options.noneValue) || REASONING_NONE_VALUE;
  const includeDefault = options.includeDefault !== false;
  const includeNone = options.includeNone === undefined
    ? hasNoneReasoningLevel(sourceLevels)
    : options.includeNone === true;
  const defaultLabel = getTrimmedString(options.defaultLabel) || "Default";
  const noneLabel = getTrimmedString(options.noneLabel) || "None";
  const uniqueLevels = uniqueReasoningLevels(sourceLevels);

  select.textContent = "";
  const optionValues = [];

  if (includeDefault) {
    appendOption(select, "", defaultLabel);
    optionValues.push("");
  }

  if (includeNone) {
    appendOption(select, noneValue, noneLabel);
    optionValues.push(noneValue);
  }

  uniqueLevels.forEach((level) => {
    appendOption(select, level, level);
    optionValues.push(level);
  });

  select.disabled = false;

  const trimmedPreferred = getTrimmedString(preferred);
  const next = includeNone && isReasoningNoneSelection(trimmedPreferred, noneValue)
    ? noneValue
    : optionValues.includes(trimmedPreferred)
    ? trimmedPreferred
    : includeDefault
    ? ""
    : optionValues[0] ?? "";
  select.value = next;
  return next;
};

export const updateReasoningSelectForModel = (select, model, preferred, options = {}) => {
  const noneValue = getTrimmedString(options.noneValue) || REASONING_NONE_VALUE;
  const rawLevels = getRawReasoningLevels(model);
  const levels = getModelReasoningLevels(model);
  const trimmedPreferred = getTrimmedString(preferred);
  const includeNone = options.includeNone === undefined
    ? modelSupportsNoneReasoning(model)
    : options.includeNone === true;
  const selectOptions = { ...options, includeNone };

  if (includeNone && isReasoningNoneSelection(trimmedPreferred, noneValue)) {
    return setReasoningOptions(select, rawLevels, noneValue, selectOptions);
  }

  if (!trimmedPreferred) {
    return setReasoningOptions(select, rawLevels.length ? rawLevels : levels, "", selectOptions);
  }

  const defaultReasoning = getModelDefaultReasoning(model);
  const selected = levels.includes(trimmedPreferred)
    ? trimmedPreferred
    : levels.includes(defaultReasoning)
    ? defaultReasoning
    : options.includeDefault === false
    ? levels[0] ?? (includeNone ? noneValue : "")
    : "";
  return setReasoningOptions(select, rawLevels.length ? rawLevels : levels, selected, selectOptions);
};

export const getReasoningEffortForChatRequest = (selected) => {
  const value = getReasoningEffortValue(selected);
  if (!value) return undefined;
  return value;
};
