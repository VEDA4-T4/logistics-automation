# Database migrations

SQLite 스키마 변경 파일을 순번 기반(`001_initial.sql`)으로 관리합니다. 대상 테이블은 `product`, `product_catalog`,
`work_history`, `image_file`, `device_status`, `error_log`, `mqtt_event_log`, `security_log`입니다.

적용된 migration의 파일명과 checksum은 `schema_migrations`에 저장됩니다. 운영 DB에 적용된 migration은
수정하지 말고 다음 순번 파일을 추가해야 합니다. 순번 누락이나 적용된 파일의 checksum 변경은 서버 시작 실패로
처리됩니다.
