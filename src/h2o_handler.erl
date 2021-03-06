%% -*- mode: erlang; tab-width: 4; indent-tabs-mode: 1; st-rulers: [70] -*-
%% vim: ts=4 sw=4 ft=erlang noet
%%%-------------------------------------------------------------------
%%% @author Andrew Bennett <andrew@pixid.com>
%%% @copyright 2015-2017, Andrew Bennett
%%% @doc
%%%
%%% @end
%%% Created :  13 Mar 2017 by Andrew Bennett <andrew@pixid.com>
%%%-------------------------------------------------------------------
-module(h2o_handler).

-include("h2o_req.hrl").

% -callback on_req(Req :: h2o_port:ref(), Host :: binary(), Path :: binary(), Opts :: any()) -> ok.

%% Public API
-export([start_link/3]).
-export([pretty_print/1]).

%% Private API
-export([init/4]).

%% Records
-record(state, {
	parent = undefined :: undefined | pid(),
	port   = undefined :: undefined | reference(),
	path   = undefined :: undefined | [binary()],
	opts   = undefined :: undefined | any()%,
	% num    = 0         :: non_neg_integer(),
	% pids   = []        :: [pid()]
}).

%%%===================================================================
%%% Public API
%%%===================================================================

start_link(Port, Path, Opts) ->
	proc_lib:start_link(?MODULE, init, [self(), Port, Path, Opts]).

%%%===================================================================
%%% Private API
%%%===================================================================

%% @private
init(Parent, Port, Path, Opts) ->
	ok = proc_lib:init_ack(Parent, {ok, self()}),
	ok = receive
		{shoot, Parent, Port} ->
			ok
	end,
	ok = h2o_nif:handler_read_start(Port),
	State = #state{parent=Parent, port=Port, path=Path, opts=Opts},
	loop(State).

%%%-------------------------------------------------------------------
%%% Internal functions
%%%-------------------------------------------------------------------

%% @private
loop(State=#state{port=Port}) ->
	receive
		{h2o_port_data, Port, ready_input} ->
			dispatch(h2o_nif:handler_read(Port), State)
	end.

pretty_print(Record) ->
	io_lib_pretty:print(Record, fun pretty_print/2).

pretty_print(h2o_req, N) ->
	N = record_info(size, h2o_req) - 1,
	record_info(fields, h2o_req);
pretty_print(_, _) ->
	[].

%% @private
dispatch([Event | Events], State=#state{opts={Handler, Opts}}) ->
	io:format("~s~n", [pretty_print(Event)]),
	% io:format("~p~n", [h2o_req:to_struct(Event)]),
	% io:format("~p~n", [h2o_req:from_struct(h2o_req:to_struct(Event))]),
% dispatch([Event | Events], State) ->
	% {ok, Pid} = Handler:start_link(Event, Opts),
	% ok = h2o_port:controlling_process(Event, Pid),
	% Pid ! {shoot, self(), Event},
	% ok = Handler:on_req(maps:from_list(Event), Opts),
	ok = Handler:on_req(Event, Opts),
	dispatch(Events, State);
	% dispatch(Events, trydispatch(State, Event));
dispatch([], State) ->
	loop(State).

% %% @private
% trydispatch(State=#state{num=Num, pids=[Pid | Pids]}, Event) ->
% 	ok = h2o_port:controlling_process(Event, Pid),
% 	Pid ! {shoot, self(), Event},
% 	State#state{num=Num - 1, pids=Pids};
% trydispatch(State=#state{num=0, pids=[]}, Event) ->
% 	trydispatch(startup(State, 10), Event).

% %% @private
% startup(State, 0) ->
% 	State;
% startup(State=#state{num=Num, pids=Pids, opts={Handler, Opts}}, N) ->
% 	{ok, Pid} = Handler:start_link(Opts),
% 	startup(State#state{num=Num + 1, pids=[Pid | Pids]}, N - 1).
