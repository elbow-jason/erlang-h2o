%% -*- mode: erlang; tab-width: 4; indent-tabs-mode: 1; st-rulers: [70] -*-
%% vim: ts=4 sw=4 ft=erlang noet
%%%-------------------------------------------------------------------
%%% @author Andrew Bennett <andrew@pixid.com>
%%% @copyright 2015-2017, Andrew Bennett
%%% @doc
%%%
%%% @end
%%% Created :  16 Feb 2017 by Andrew Bennett <andrew@pixid.com>
%%%-------------------------------------------------------------------
-module(h2o_driver).
-behaviour(gen_server).

-include("h2o.hrl").

%% Public API
-export([start_link/0]).

%% gen_server callbacks
-export([init/1]).
-export([handle_call/3]).
-export([handle_cast/2]).
-export([handle_info/2]).
-export([terminate/2]).
-export([code_change/3]).

%% Private API
-export([load/0]).
-export([unload/0]).

%% Records
-record(state, {
	port = undefined :: undefined | port()
}).

%%%===================================================================
%%% Public API
%%%===================================================================

-spec start_link()
	-> {ok, pid()} | ignore | {error, term()}.
start_link() ->
	gen_server:start_link({local, ?MODULE}, ?MODULE, [], []).

%%%===================================================================
%%% gen_server callbacks
%%%===================================================================

-spec init([])
	-> ignore | {ok, #state{}} | {stop, any()}.
init([]) ->
	erlang:process_flag(trap_exit, true),
	case load() of
		ok ->
			Port = erlang:open_port({spawn_driver, ?H2O_DRIVER_NAME}, [binary]),
			erlang:register(?H2O_DRIVER_ATOM, Port),
			State = #state{port=Port},
			{ok, State};
		{error, LoadError} ->
			LoadErrorStr = erl_ddll:format_error(LoadError),
			ErrorStr = lists:flatten(io_lib:format(
				"could not load driver ~s: ~p",
				[?H2O_DRIVER_NAME, LoadErrorStr])),
			{stop, ErrorStr}
	end.

-spec handle_call(any(), {pid(), any()}, #state{})
	-> {reply, any(), #state{}}.
handle_call(_Request, _From, State) ->
	{reply, ok, State}.

-spec handle_cast(any(), #state{})
	-> {noreply, #state{}} | {stop, any(), #state{}}.
handle_cast(stop, State) ->
	{stop, normal, State};
handle_cast(_Msg, State) ->
	{noreply, State}.

-spec handle_info(any(), #state{})
	-> {noreply, #state{}}.
handle_info(_Info, State) ->
	{noreply, State}.

-spec terminate(any(), #state{})
	-> ok.
terminate(_Reason, #state{port=Port}) ->
	erlang:unregister(?H2O_DRIVER_ATOM),
	erlang:port_close(Port),
	ok.

-spec code_change(any(), #state{}, any())
	-> {ok, #state{}}.
code_change(_OldVsn, State, _Extra) ->
	{ok, State}.

%%%===================================================================
%%% Private API
%%%===================================================================

%%--------------------------------------------------------------------
%% @private
%% @doc Load port driver
%% @spec load() -> ok | {error, Error}
%% @end
%%--------------------------------------------------------------------
load() ->
	{ok, Drivers} = erl_ddll:loaded_drivers(),
	case lists:member(?H2O_DRIVER_NAME, Drivers) of
		true ->
			ok;
		false ->
			case erl_ddll:load(priv_dir(), ?H2O_DRIVER_NAME) of
				ok ->
					ok;
				{error, already_loaded} ->
					ok;
				{error, Error} ->
					error_logger:error_msg(
						?MODULE_STRING ": Error loading ~p: ~p~n",
						[?H2O_DRIVER_NAME, erl_ddll:format_error(Error)]
					),
					{error, Error}
			end
	end.

%%--------------------------------------------------------------------
%% @private
%% @doc Unload port driver
%% @spec unload() -> ok | {error, Error}
%% @end
%%--------------------------------------------------------------------
unload() ->
	case erl_ddll:unload_driver(?H2O_DRIVER_NAME) of
		ok ->
			ok;
		{error, Error} ->
			error_logger:error_msg(
				?MODULE_STRING ": Error unloading ~p: ~p~n",
				[?H2O_DRIVER_NAME, erl_ddll:format_error(Error)]
			),
			{error, Error}
	end.

%%%-------------------------------------------------------------------
%%% Internal functions
%%%-------------------------------------------------------------------

%% @private
priv_dir() ->
	case code:priv_dir(h2o) of
		{error, bad_name} ->
			case code:which(?MODULE) of
				Filename when is_list(Filename) ->
					filename:join([filename:dirname(Filename), "../priv"]);
				_ ->
					"../priv"
			end;
		Dir ->
			Dir
	end.
