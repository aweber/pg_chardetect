Test, test, test...
===================

To test the pg_chardet functions:

1.  Follow the instructions in ../README.md to build and install the pg_chardet extension
2.  Create a test db:

```bash
sudo su - postgres
createdb test
```

3.  Install the test data in this diretory:

```bash
zcat test.dump_p.gz | psql test
```

4.  Run the pg_chardet_test.sql script to see pg_chardet in action!

```bash
psql test --single-step --file pg_chardet_test.sql
```
