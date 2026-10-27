/// The source public router removes every trailing slash except the root slash.
pub fn normalized_path(path: &str) -> &str {
    if path == "/" {
        path
    } else {
        path.trim_end_matches('/')
    }
}

#[cfg(test)]
mod route_tests {
    #[test]
    fn normalization_matches_source_without_decoding_segments() {
        for (source, expected) in [
            ("/", "/"),
            ("/health///", "/health"),
            ("/v1/responses/", "/v1/responses"),
            ("/api/auth/login/start//", "/api/auth/login/start"),
            ("/v1/live/rtc_%2F/", "/v1/live/rtc_%2F"),
            ("///", ""),
        ] {
            assert_eq!(super::normalized_path(source), expected);
        }
    }
}
