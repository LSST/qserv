define([
    'modules/sql-formatter.min',
    'underscore'],

function(sqlFormatter,
         _) {

    class Common {
        static RestAPIVersion = 24;
        static query2text(query, expanded) {
            if (expanded) {
                return sqlFormatter.format(query, Common._sqlFormatterConfig);
            } else if (query.length > Common._max_compact_length) {
                return query.substring(0, Common._max_compact_length) + "...";
            } else {
                return query;
            }
        }
        static _sqlFormatterConfig = {"language":"mysql", "uppercase:":true, "indent":"  "};
        static _max_compact_length = 120;
        static _ivals = [
            {value:   2, name:  '2 sec'},
            {value:   5, name:  '5 sec'},
            {value:  10, name: '10 sec'},
            {value:  20, name: '20 sec'},
            {value:  30, name: '30 sec'},
            {value:  60, name:  '1 min'},
            {value: 120, name:  '2 min'},
            {value: 300, name:  '5 min'},
            {value: 600, name: '10 min'}
        ];
        static html_update_ival(id, default_ival = 30, ivals = undefined) {
            return `
<label for="${id}"><i class="bi bi-arrow-repeat"></i>&nbsp;interval:</label>
<select id="${id}" class="form-control form-control-selector">` + _.reduce(ivals || Common._ivals, function (html, ival) { return html + `
  <option value="${ival.value}" ${ival.value == default_ival ? 'selected' : ''}>${ival.name}</option>`;
            }, '') + `
</select>`;
        }
    }
    return Common;
});
