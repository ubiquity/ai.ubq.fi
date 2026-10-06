> ## Documentation Index
>
> Fetch the complete documentation index at: https://docs.lithosai.com/llms.txt Use this file to discover all available
> pages before exploring further.

# Billing

LithosAI inference service is prepaid. Each request is metered to the token and debited from your organization's
balance.

## Rates

Each model carries three rates per million tokens: input, cached input and output. Cached input is a subset of input
rather than an addition to it, so a request is charged the input rate for `prompt_tokens - cached_tokens`, the cached
rate for `cached_tokens`, and the output rate for `completion_tokens`.

## Running out

At a zero balance, requests return 402 with code `insufficient_quota` and `x-should-retry: false`. Top up from the
[Billing](https://console.lithosai.cloud/billing) page, between \$5 and \$5,000 a purchase.

## Automatic reload

The platform supports automatic reload. Set a threshold, a target and a monthly cap: when the balance falls below the
threshold we charge your saved card enough to restore the target, and never more than the cap in one calendar month. If
a charge fails, reload disarms itself and your organization's admins are emailed rather than the card being retried
indefinitely.

## Reporting

Spend and token counts are broken down by day, model and API key on the
[Billing](https://console.lithosai.cloud/billing) and [Analytics](https://console.lithosai.cloud/analytics) pages.

This documentation is built and hosted on [Mintlify](https://mintlify.com), a developer documentation platform.
