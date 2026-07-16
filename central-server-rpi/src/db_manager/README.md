# Database manager

중앙 서버의 SQLite 연결, migration, 트랜잭션, repository를 제공합니다. 중앙관제와 장치 노드는 DB에 직접
접근하지 않습니다.

- `Database`/`Statement`/`Transaction`: SQLite C API의 RAII 래퍼
- `MigrationRunner`: 순번 SQL 적용 및 적용된 파일의 checksum 검증
- `PersistenceService`: 검증된 MQTT envelope와 typed payload의 멱등 저장 진입점
- `ImageStore`: 이미지의 SHA-256 경로 생성과 임시 파일 기반 원자적 저장
- `RetentionService`: 설정된 보존 기간에 따른 batch 정리

SQLite는 foreign key, WAL, `synchronous=NORMAL`을 사용합니다. 재수신된 `messageId`는 파생 데이터를 다시
저장하지 않고 MQTT 감사 로그의 중복 횟수만 증가시킵니다.
