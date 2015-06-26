SET statement_timeout = 0;
SET client_encoding = 'SQL_ASCII';
SET standard_conforming_strings = off;
SET check_function_bodies = false;
SET client_min_messages = warning;
SET escape_string_warning = off;

SET search_path = public, pg_catalog;

--
-- Name: force_conversion(); Type: FUNCTION; Schema: public; Owner: postgres
--

CREATE FUNCTION force_conversion() RETURNS trigger
    LANGUAGE plpgsql
    AS $$
begin
  new.convert_this := convert_to_utf8(new.convert_this, true);
  return new;
end;
$$;


ALTER FUNCTION public.force_conversion() OWNER TO postgres;


--
-- Name: force_conversion_on_insert; Type: TRIGGER; Schema: public; Owner: postgres
--

CREATE TRIGGER force_conversion_on_insert BEFORE INSERT ON test FOR EACH ROW EXECUTE PROCEDURE force_conversion();


--
-- Name: force_conversion_on_update; Type: TRIGGER; Schema: public; Owner: postgres
--

-- CREATE TRIGGER force_conversion_on_update BEFORE UPDATE ON test FOR EACH ROW WHEN ((new.convert_this IS DISTINCT FROM old.convert_this)) EXECUTE PROCEDURE force_conversion();
CREATE TRIGGER force_conversion_on_update BEFORE UPDATE ON test FOR EACH ROW EXECUTE PROCEDURE force_conversion();

--
-- Check the data
--

select * from test order by 1,2;

--
-- use char_set_detect() to guess the charset
--

select *, char_set_detect(convert_this) from test order by 1, 2;

--
--  use the convert_to_utf8(text_in text, src_encoding text) plpgsql function to convert the data using buiilt-in PG convert* functions
--

select * convert_to_utf8(convert_this, (char_set_detect(convert_this)).encoding) from test order by 1, 2;

--
--  use convert_to_utf8(text, boolean) C function to convert data
--

select * convert_to_utf8(convert_this, true) from test order by 1, 2;

--
-- demonstrate in-place conversion using update trigger function
--

select * from test order by 1, 2;

update test set convert_this = convert_this;

select * from test order by 1, 2;




