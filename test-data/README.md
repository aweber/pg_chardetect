Test, test, test...
===================

To test the pg_chardet functions:

1. Follow the instructions in ../README.md to build and install the pg_chardet extension
2. Create a test db:

    ```bash
    sudo su - postgres
    createdb test
    ```

3. Install the test data in this directory:

    ```bash
    zcat test.dump_p.gz | psql test
    ```

4. Install the pg_chardetect database functions:

    ```bash
    psql test  -f $(pg_config --sharedir)/contrib/pg_chardetect.sql
    ```

5. Run the pg_chardetect_test.sql script to see pg_chardetect in action!

    ```bash
    psql test --file pg_chardetect_test.sql
    ```
